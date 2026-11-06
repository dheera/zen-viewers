/* SPDX-License-Identifier: GPL-3.0-or-later
 * Page motion only changes scene transforms. Poppler raster jobs stay on workers.
 */
static gboolean animations_enabled(App *a) {
    gboolean enabled=TRUE;
    g_object_get(gtk_widget_get_settings(a->canvas),"gtk-enable-animations",&enabled,NULL);
    return enabled;
}
static double rubber_offset(double distance) {
    return copysign(72*(1-exp(-fabs(distance)/150)),distance);
}
static void view_stop(App *a) {
    if (a->motion_tick) { gtk_widget_remove_tick_callback(a->canvas,a->motion_tick); a->motion_tick=0; }
    a->flipping=FALSE; a->flip_start=0; a->flip_progress=1; a->elastic=0;
    a->target_offset=a->offset; a->tick_time=0; g_clear_object(&a->outgoing);
}
static gboolean view_tick(GtkWidget *widget,GdkFrameClock *clock,gpointer data) {
    App *a=data;
    gint64 now=gdk_frame_clock_get_frame_time(clock);
    double dt=a->tick_time ? CLAMP((now-a->tick_time)/1000000.,.001,.05) : 1./60;
    a->tick_time=now;
    gboolean animate=animations_enabled(a);
    double mix=animate ? 1-exp(-dt*20) : 1;
    a->offset += (a->target_offset-a->offset)*mix;
    if (fabs(a->target_offset-a->offset)<.25) a->offset=a->target_offset;
    a->elastic *= animate ? exp(-dt*13) : 0;
    if (fabs(a->elastic)<.25) a->elastic=0;
    if (a->flipping && !cached(a,a->page,0,FALSE)) {
        if (a->failed_page == a->page) {
            a->flipping=FALSE; g_clear_object(&a->outgoing);
        } else {
            /* Keep the outgoing page still while a slow raster completes. The
             * worker completion restarts animation; do not spin a frame clock. */
            a->motion_tick=0; a->tick_time=0; return G_SOURCE_REMOVE;
        }
    }
    if (a->flipping) {
        if (!a->flip_start) a->flip_start=now;
        double t=animate ? CLAMP((now-a->flip_start)/230000.,0,1) : 1;
        a->flip_progress=1-pow(1-t,3);
        if (t>=1) { a->flipping=FALSE; g_clear_object(&a->outgoing); }
    }
    gtk_widget_queue_draw(widget);
    if (!a->flipping && a->offset==a->target_offset && a->elastic==0) {
        a->motion_tick=0; a->tick_time=0; return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}
static void view_start(App *a) {
    if (!a->motion_tick) a->motion_tick=gtk_widget_add_tick_callback(a->canvas,view_tick,a,NULL);
}
static void view_navigate(App *a,int page,int axis,gboolean at_bottom) {
    if (!a->doc) return;
    page=CLAMP(page,0,(int)a->doc->sizes->len-1);
    if (page==a->page) { view_start(a); return; }
    gboolean animate=animations_enabled(a);
    if (!animate) g_clear_object(&a->outgoing);
    if (!a->flipping) {
        Raster *old=cached(a,a->page,0,FALSE);
        if (old && animate) {
            Layout l=current_layout(a);
            g_set_object(&a->outgoing,old->texture);
            memcpy(a->outgoing_edge,old->edge,sizeof old->edge);
            a->outgoing_x=l.x; a->outgoing_y=l.y;
            a->outgoing_w=l.w; a->outgoing_h=l.h;
        }
    }
    a->flip_sign=page>a->page ? 1 : -1;
    a->travel_direction=a->flip_sign; a->flip_axis=axis;
    a->flipping=a->outgoing!=NULL && animate;
    a->flip_start=0; a->flip_progress=0; a->elastic=0;
    selection_reset(a); a->page=page; a->offset=0; a->offset_x=0;
    if (at_bottom) a->offset=current_layout(a).max_offset;
    a->target_offset=a->offset; a->failed_page=-1;
    refresh(a); view_start(a);
}
static void view_scroll(App *a,double amount) {
    if (!a->doc || amount==0) return;
    if (!a->motion_tick && !a->flipping) a->target_offset=a->offset;
    double maximum=current_layout(a).max_offset;
    double next=a->target_offset+amount;
    if (!a->flipping && next>maximum && a->target_offset>=maximum-.5 &&
        a->page+1<(int)a->doc->sizes->len) {
        view_navigate(a,a->page+1,1,FALSE); return;
    }
    if (!a->flipping && next<0 && a->target_offset<=.5 && a->page>0) {
        view_navigate(a,a->page-1,1,TRUE); return;
    }
    a->target_offset=CLAMP(next,0,maximum);
    if ((next<0 && a->page==0) || (next>maximum && a->page+1==(int)a->doc->sizes->len)) {
        if (animations_enabled(a)) a->elastic=CLAMP(a->elastic+rubber_offset(a->target_offset-next),-72,72);
    }
    view_start(a);
}

typedef struct { GtkWidget parent; App *app; } PdfCanvas;
typedef struct { GtkWidgetClass parent; } PdfCanvasClass;
G_DEFINE_TYPE(PdfCanvas,pdf_canvas,GTK_TYPE_WIDGET)

static void color_rect(GtkSnapshot *snapshot,GdkRGBA color,double x,double y,double w,double h) {
    if (w<=0 || h<=0) return;
    graphene_rect_t rect=GRAPHENE_RECT_INIT(x,y,w,h);
    gtk_snapshot_append_color(snapshot,&color,&rect);
}
static void page_snapshot(GtkSnapshot *snapshot,GdkTexture *texture,Layout l,
                          const GdkRGBA edge[4],int width,int height,double dx,double dy) {
    /* Each slide carries its own viewport backing so changing paper colors do not flash. */
    graphene_rect_t viewport=GRAPHENE_RECT_INIT(dx,dy,width,height);
    gtk_snapshot_push_clip(snapshot,&viewport);
    color_rect(snapshot,edge[0],dx,dy,width,height);
    color_rect(snapshot,edge[2],dx,l.y+l.h,width,MAX(0,dy+height-l.y-l.h));
    color_rect(snapshot,edge[3],dx,l.y,MAX(0,l.x-dx),l.h);
    color_rect(snapshot,edge[1],l.x+l.w,l.y,MAX(0,dx+width-l.x-l.w),l.h);
    graphene_rect_t paper=GRAPHENE_RECT_INIT(l.x,l.y,l.w,l.h);
    gtk_snapshot_push_clip(snapshot,&paper);
    if (texture) {
        graphene_rect_t image=GRAPHENE_RECT_INIT(l.x,l.y,l.w,
            l.w*gdk_texture_get_height(texture)/gdk_texture_get_width(texture));
        gtk_snapshot_append_texture(snapshot,texture,&image);
    } else {
        const GdkRGBA white={1,1,1,1};
        gtk_snapshot_append_color(snapshot,&white,&paper);
    }
    gtk_snapshot_pop(snapshot); gtk_snapshot_pop(snapshot);
}
static void canvas_snapshot(GtkWidget *widget,GtkSnapshot *snapshot) {
    App *a=((PdfCanvas*)widget)->app;
    int width=gtk_widget_get_width(widget),height=gtk_widget_get_height(widget);
    if (a->width!=width || a->height!=height) {
        a->width=width; a->height=height;
        if (!a->layout_timer) a->layout_timer=g_idle_add(update_controls,a);
        if (a->doc) {
            clamp_offsets(a); a->target_offset=CLAMP(a->target_offset,0,current_layout(a).max_offset);
            request_render(a);
        }
    }
    const GdkRGBA neutral={.96,.96,.95,1};
    color_rect(snapshot,neutral,0,0,width,height);
    if (!a->doc) return;
    graphene_rect_t viewport=GRAPHENE_RECT_INIT(0,0,width,height);
    gtk_snapshot_push_clip(snapshot,&viewport);
    Raster *r=cached(a,a->page,desired_pixels(a),FALSE);
    Layout l=current_layout(a);
    double dx=0,dy=0;
    if (a->flipping) {
        if (a->flip_axis) dy=a->flip_sign*(1-a->flip_progress)*height;
        else dx=a->flip_sign*(1-a->flip_progress)*width;
    }
    GdkRGBA paper_edges[4]={{1,1,1,1},{1,1,1,1},{1,1,1,1},{1,1,1,1}};
    page_snapshot(snapshot,r?r->texture:NULL,l,r?r->edge:paper_edges,width,height,dx,dy);
    if (a->flipping && a->outgoing) {
        double oldx=a->flip_axis?0:-a->flip_sign*a->flip_progress*width;
        double oldy=a->flip_axis?-a->flip_sign*a->flip_progress*height:0;
        Layout old={.x=a->outgoing_x+oldx,.y=a->outgoing_y+oldy,
                    .w=a->outgoing_w,.h=a->outgoing_h};
        page_snapshot(snapshot,a->outgoing,old,a->outgoing_edge,width,height,oldx,oldy);
    }
    if (r) {
        if (a->selection.start<a->selection.end) {
            cairo_t *cr=gtk_snapshot_append_cairo(snapshot,&viewport);
            selection_draw(a,cr,l); cairo_destroy(cr);
        }
        selection_schedule(a);
    }
    gtk_snapshot_pop(snapshot);
}
static void pdf_canvas_class_init(PdfCanvasClass *klass) {
    GTK_WIDGET_CLASS(klass)->snapshot=canvas_snapshot;
}
static void pdf_canvas_init(PdfCanvas *self) { (void)self; }
static GtkWidget *pdf_canvas_new(App *a) {
    PdfCanvas *canvas=g_object_new(pdf_canvas_get_type(),NULL);
    canvas->app=a; return GTK_WIDGET(canvas);
}

/* Outline extraction is opt-in, bounded, and serialized with the document's
 * raster/text workers. No Poppler operation runs in the UI callbacks below. */
static void outline_walk(Document *doc,PopplerIndexIter *iter,GPtrArray *entries,int depth,guint *visited) {
    if (!iter) return;
    do {
        if (++*visited>10000) break;
        PopplerAction *action=poppler_index_iter_get_action(iter);
        if (action && action->type==POPPLER_ACTION_GOTO_DEST) {
            PopplerDest *dest=action->goto_dest.dest, *resolved=NULL;
            if (dest && dest->type==POPPLER_DEST_NAMED && dest->named_dest)
                dest=resolved=poppler_document_find_dest(doc->pdf,dest->named_dest);
            if (dest && dest->type!=POPPLER_DEST_NAMED && dest->page_num>=1 &&
                dest->page_num<=(int)doc->sizes->len) {
                OutlineEntry *entry=g_new0(OutlineEntry,1);
                char *bounded=g_strndup(action->any.title ? action->any.title : "Untitled section",4096);
                char *valid=g_utf8_make_valid(bounded,-1); g_free(bounded);
                entry->title=g_utf8_substring(valid,0,MIN(g_utf8_strlen(valid,-1),512));
                g_free(valid);
                for (char *c=entry->title; *c; c++) if ((guchar)*c<32) *c=' ';
                entry->page=dest->page_num-1; entry->depth=depth;
                g_ptr_array_add(entries,entry);
            }
            if (resolved) poppler_dest_free(resolved);
        }
        if (action) poppler_action_free(action);
        if (depth<31 && *visited<10000)
            outline_walk(doc,poppler_index_iter_get_child(iter),entries,depth+1,visited);
    } while (*visited<10000 && poppler_index_iter_next(iter));
    poppler_index_iter_free(iter);
}
static void outline_thread(GTask *task,gpointer source,gpointer data,GCancellable *cancel) {
    (void)source; (void)cancel; Document *doc=data;
    GPtrArray *entries=g_ptr_array_new_with_free_func(outline_entry_free);
    guint visited=0;
    g_mutex_lock(&doc->pdf_lock);
    outline_walk(doc,poppler_index_iter_new(doc->pdf),entries,0,&visited);
    g_mutex_unlock(&doc->pdf_lock);
    g_task_return_pointer(task,entries,(GDestroyNotify)g_ptr_array_unref);
}
static void outline_done(GObject *source,GAsyncResult *result,gpointer data) {
    (void)source; App *a=data;
    Document *doc=g_task_get_task_data(G_TASK(result));
    doc->outline=g_task_propagate_pointer(G_TASK(result),NULL);
    doc->outline_loading=FALSE; doc->outline_loaded=TRUE;
    if (!a->closed && a->doc==doc) sidebar_reset(a);
    g_application_release(G_APPLICATION(a->application));
}
static void sidebar_load(App *a) {
    if (!a->doc || a->doc->outline_loading || a->doc->outline_loaded ||
        !cached(a,a->page,0,FALSE)) return;
    a->doc->outline_loading=TRUE;
    GTask *task=g_task_new(a->window,NULL,outline_done,a);
    g_task_set_task_data(task,document_ref(a->doc),(GDestroyNotify)document_unref);
    g_application_hold(G_APPLICATION(a->application));
    g_task_run_in_thread(task,outline_thread); g_object_unref(task);
}
static gboolean sidebar_has_outline(App *a) {
    return a->doc && a->doc->outline && a->doc->outline->len;
}
static void sidebar_setup(GtkSignalListItemFactory *factory,GObject *object,gpointer data) {
    (void)factory; (void)data;
    GtkWidget *label=gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label),0);
    gtk_label_set_ellipsize(GTK_LABEL(label),PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(label,18); gtk_widget_set_margin_end(label,18);
    gtk_widget_set_margin_top(label,12); gtk_widget_set_margin_bottom(label,12);
    gtk_list_item_set_child(GTK_LIST_ITEM(object),label);
}
static void sidebar_bind(GtkSignalListItemFactory *factory,GObject *object,gpointer data) {
    (void)factory; (void)data;
    GtkListItem *item=GTK_LIST_ITEM(object);
    GtkStringObject *name=gtk_list_item_get_item(item);
    GtkWidget *label=gtk_list_item_get_child(item);
    gtk_label_set_text(GTK_LABEL(label),gtk_string_object_get_string(name));
    gtk_widget_set_tooltip_text(label,gtk_string_object_get_string(name));
}
static void sidebar_activated(GtkListView *list,guint position,gpointer data) {
    (void)list; App *a=data;
    int page=(int)position;
    if (sidebar_has_outline(a)) {
        if (position>=a->doc->outline->len) return;
        page=((OutlineEntry*)g_ptr_array_index(a->doc->outline,position))->page;
    }
    go_page(a,page); gtk_widget_grab_focus(a->canvas);
}
static void sidebar_update(App *a) {
    if (!a->page_selection || !a->doc) return;
    guint selected=(guint)a->page;
    if (sidebar_has_outline(a)) {
        int nearest=-1; selected=GTK_INVALID_LIST_POSITION;
        for (guint i=0;i<a->doc->outline->len;i++) {
            OutlineEntry *entry=g_ptr_array_index(a->doc->outline,i);
            if (entry->page<=a->page && entry->page>nearest) { nearest=entry->page; selected=i; }
        }
    }
    gtk_single_selection_set_selected(a->page_selection,selected);
}
static void sidebar_populate(App *a) {
    if (!a->doc || g_list_model_get_n_items(G_LIST_MODEL(a->page_names))) return;
    gboolean contents=sidebar_has_outline(a);
    guint n=contents ? a->doc->outline->len : a->doc->sizes->len;
    char **names=g_new0(char*,n+1);
    for (guint i=0;i<n;i++) {
        if (contents) {
            OutlineEntry *entry=g_ptr_array_index(a->doc->outline,i);
            names[i]=g_strdup_printf("%*s%s · %d",MIN(entry->depth,6)*2,"",entry->title,entry->page+1);
        } else names[i]=g_strdup_printf("Page %u",i+1);
    }
    gtk_label_set_text(GTK_LABEL(a->sidebar_title),contents?"Contents":"Pages");
    gtk_string_list_splice(a->page_names,0,0,(const char*const*)names);
    g_strfreev(names); sidebar_update(a); sidebar_load(a);
}
static void sidebar_reset(App *a) {
    gtk_string_list_splice(a->page_names,0,g_list_model_get_n_items(G_LIST_MODEL(a->page_names)),NULL);
    if (a->sidebar_visible) sidebar_populate(a);
}
static void sidebar_toggle(App *a) {
    a->sidebar_visible=!a->sidebar_visible;
    if (a->sidebar_visible) sidebar_populate(a);
    gtk_revealer_set_reveal_child(GTK_REVEALER(a->sidebar),a->sidebar_visible);
    if (!a->sidebar_visible) gtk_widget_grab_focus(a->canvas);
}
static void sidebar_clicked(GtkButton *button,gpointer data) { (void)button; sidebar_toggle(data); }
static void make_sidebar(App *a) {
    a->content=gtk_overlay_new(); gtk_overlay_set_child(GTK_OVERLAY(a->content),a->canvas);
    GtkWidget *box=gtk_box_new(GTK_ORIENTATION_VERTICAL,4);
    gtk_widget_add_css_class(box,"pdf-pages");
    GtkWidget *heading=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    GtkWidget *title=gtk_label_new("Pages"); a->sidebar_title=title;
    gtk_widget_set_hexpand(title,TRUE); gtk_label_set_xalign(GTK_LABEL(title),0);
    gtk_widget_set_margin_start(heading,16); gtk_widget_set_margin_end(heading,8);
    gtk_widget_set_margin_top(heading,8);
    gtk_box_append(GTK_BOX(heading),title);
    gtk_box_append(GTK_BOX(heading),zen_button_new("window-close-symbolic","Hide contents and pages (Ctrl+B)",G_CALLBACK(sidebar_clicked),a));
    gtk_box_append(GTK_BOX(box),heading);
    a->page_names=gtk_string_list_new(NULL);
    a->page_selection=gtk_single_selection_new(G_LIST_MODEL(g_object_ref(a->page_names)));
    GtkListItemFactory *factory=gtk_signal_list_item_factory_new();
    g_signal_connect(factory,"setup",G_CALLBACK(sidebar_setup),a);
    g_signal_connect(factory,"bind",G_CALLBACK(sidebar_bind),a);
    a->sidebar_list=gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(a->page_selection)),factory);
    gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(a->sidebar_list),TRUE);
    g_signal_connect(a->sidebar_list,"activate",G_CALLBACK(sidebar_activated),a);
    GtkWidget *scroll=gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),a->sidebar_list);
    gtk_widget_set_vexpand(scroll,TRUE); gtk_box_append(GTK_BOX(box),scroll);
    a->sidebar=gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(a->sidebar),GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
    gtk_revealer_set_transition_duration(GTK_REVEALER(a->sidebar),180);
    gtk_revealer_set_child(GTK_REVEALER(a->sidebar),box);
    gtk_widget_set_halign(a->sidebar,GTK_ALIGN_START); gtk_widget_set_valign(a->sidebar,GTK_ALIGN_FILL);
    gtk_widget_set_size_request(box,196,-1);
    gtk_widget_set_margin_top(a->sidebar,76); gtk_widget_set_margin_bottom(a->sidebar,76);
    gtk_widget_set_margin_start(a->sidebar,10);
    gtk_overlay_add_overlay(GTK_OVERLAY(a->content),a->sidebar);
    GtkCssProvider *css=gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        ".pdf-pages {background: alpha(#202723,.97); color:#eff3ee; border-radius:14px;}"
        ".pdf-pages listview {background:transparent;color:#eff3ee;}"
        ".pdf-pages row:selected {background:alpha(#bdd7c8,.24);}"
        ".pdf-pages button {background:transparent;color:#eff3ee;}",-1);
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(a->canvas),GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}
