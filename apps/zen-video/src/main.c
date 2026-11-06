/* SPDX-License-Identifier: GPL-3.0-or-later */
#define GDK_VERSION_MIN_REQUIRED GDK_VERSION_4_6
#define GDK_VERSION_MAX_ALLOWED GDK_VERSION_4_6
#include "zen-ui.h"
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#ifdef GDK_WINDOWING_X11
#include <epoxy/glx.h>
#include <gdk/x11/gdkx.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <math.h>
#include <string.h>

#ifndef ZEN_VERSION
#define ZEN_VERSION "0.5.0"
#endif

typedef struct {
    gint refs, closing, queued;
    ZenWindow *shell;
    GtkWindow *window;
    GtkGLArea *area;
    GdkGLContext *gl_context;
    GtkWidget *play, *mute, *timeline, *time, *subtitles, *fullscreen;
    GtkWidget *subtitle_name, *subtitle_toggle, *audio_label;
    GtkFileChooserNative *subtitle_dialog;
    mpv_handle *mpv;
    mpv_render_context *render;
    char *path, *subtitle_path, *caption, *audio_output;
    gboolean loaded, has_video, has_audio, paused, muted, eof, seekable;
    gboolean updating, scrubbing, sub_visible, quiet_audio, audio_warned;
    gboolean recovering, restore_pause;
    double position, duration, volume, seek_target, restore_position;
    guint seek_timer, layout_idle;
    gint64 last_ui;
    int subtitle_count, logged_second;
    guint64 load_id;
} Video;
static gboolean diagnostics;
static void update_ui(Video *v,gboolean force);
static void load_pending(Video *v);
static void choose_subtitles(Video *v);
static void wakeup(void *data);
static void release_render(Video *v);

static Video *video_ref(Video *v) { g_atomic_int_inc(&v->refs); return v; }
static void video_unref(gpointer data) {
    Video *v=data;
    if (!g_atomic_int_dec_and_test(&v->refs)) return;
    g_free(v->path); g_free(v->subtitle_path); g_free(v->caption); g_free(v->audio_output); g_free(v);
}
static void notice(Video *v,const char *message) {
    if (!g_atomic_int_get(&v->closing)) zen_window_notice(v->shell,message);
}
static void command(Video *v,const char **args) {
    if (!v->mpv || g_atomic_int_get(&v->closing)) return;
    int error=mpv_command_async(v->mpv,0,args);
    if (error<0) notice(v,mpv_error_string(error));
}
static void flag(Video *v,const char *name,gboolean value) {
    if (v->mpv) { int data=value; mpv_set_property_async(v->mpv,0,name,MPV_FORMAT_FLAG,&data); }
}
static void number(Video *v,const char *name,double value) {
    if (v->mpv) mpv_set_property_async(v->mpv,0,name,MPV_FORMAT_DOUBLE,&value);
}
static char *short_text(const char *text) {
    char *bounded=g_strndup(text?text:"",4096),*valid=g_utf8_make_valid(bounded,-1); g_free(bounded);
    char *result=g_utf8_substring(valid,0,MIN(g_utf8_strlen(valid,-1),200));g_free(valid);return result;
}
static void time_text(double value,char out[32]) {
    if (!isfinite(value) || value<0) { g_strlcpy(out,"–:––",32); return; }
    guint64 seconds=(guint64)MIN(value,359999999.);
    if (seconds>=3600) g_snprintf(out,32,"%" G_GUINT64_FORMAT ":%02u:%02u",seconds/3600,(guint)(seconds/60%60),(guint)(seconds%60));
    else g_snprintf(out,32,"%u:%02u",(guint)(seconds/60),(guint)(seconds%60));
}
static void update_ui(Video *v,gboolean force) {
    if (g_atomic_int_get(&v->closing)) return;
    gint64 now=g_get_monotonic_time();
    if (!force && now-v->last_ui<100000) return;
    v->last_ui=now; v->updating=TRUE;
    gtk_button_set_icon_name(GTK_BUTTON(v->play),v->paused||v->eof?"media-playback-start-symbolic":"media-playback-pause-symbolic");
    gtk_widget_set_tooltip_text(v->play,v->eof?"Replay (Space)":v->paused?"Play (Space)":"Pause (Space)");
    gtk_button_set_icon_name(GTK_BUTTON(v->mute),v->muted||v->volume<=0?"audio-volume-muted-symbolic":"audio-volume-high-symbolic");
    gtk_widget_set_tooltip_text(v->mute,v->muted?"Unmute (M)":"Mute (M)");
    gtk_widget_set_sensitive(v->play,v->loaded);
    gtk_widget_set_sensitive(v->mute,v->loaded&&v->has_audio);
    gtk_widget_set_sensitive(v->subtitles,v->loaded&&v->has_video);
    gtk_widget_set_sensitive(v->timeline,v->loaded&&v->seekable&&v->duration>0);
    if (!v->scrubbing && !v->seek_timer) {
        gtk_range_set_range(GTK_RANGE(v->timeline),0,MAX(v->duration,1));
        gtk_range_set_value(GTK_RANGE(v->timeline),CLAMP(v->position,0,MAX(v->duration,1)));
    }
    char elapsed[32],duration[32],label[70];
    time_text(v->scrubbing?v->seek_target:v->position,elapsed);time_text(v->duration>0?v->duration:-1,duration);
    if (gtk_widget_get_width(GTK_WIDGET(v->area))>=580) g_snprintf(label,sizeof label,"%s / %s",elapsed,duration);
    else g_strlcpy(label,elapsed,sizeof label);
    gtk_label_set_text(GTK_LABEL(v->time),label);
    gtk_label_set_text(GTK_LABEL(v->subtitle_name),!v->sub_visible?"Subtitles hidden":v->caption?v->caption:"No subtitle selected");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(v->subtitle_toggle),v->sub_visible);
    gtk_widget_set_visible(v->audio_label,v->loaded&&v->has_audio&&!v->has_video);
    v->updating=FALSE;
    if (v->loaded&&v->has_audio&&!v->quiet_audio&&!v->audio_warned&&g_strcmp0(v->audio_output,"null")==0) {
        notice(v,"Audio output is unavailable. Video will continue silently.");v->audio_warned=TRUE;
    }
}
static void seek(Video *v,double seconds,gboolean absolute,gboolean exact) {
    if (!v->loaded || !v->seekable) return;
    char value[G_ASCII_DTOSTR_BUF_SIZE];g_ascii_dtostr(value,sizeof value,seconds);
    const char *args[]={"seek",value,absolute?(exact?"absolute+exact":"absolute"):(exact?"relative+exact":"relative"),NULL};
    command(v,args);
    if (diagnostics) g_printerr("zen-video seek %s %s\n",value,args[2]);
}
static gboolean commit_seek(gpointer data) {
    Video *v=data;v->seek_timer=0;
    if (!g_atomic_int_get(&v->closing)) seek(v,v->seek_target,TRUE,!v->scrubbing);
    return G_SOURCE_REMOVE;
}
static void seek_changed(GtkRange *range,gpointer data) {
    Video *v=data;if(v->updating||!v->loaded)return;
    v->seek_target=gtk_range_get_value(range);
    if(!v->seek_timer)v->seek_timer=g_timeout_add(60,commit_seek,v);
}
static gboolean seek_event(GtkEventControllerLegacy *controller,GdkEvent *event,gpointer data) {
    (void)controller;Video *v=data;GdkEventType type=gdk_event_get_event_type(event);
    if(type==GDK_BUTTON_PRESS||type==GDK_TOUCH_BEGIN)v->scrubbing=TRUE;
    else if(type==GDK_BUTTON_RELEASE||type==GDK_TOUCH_END||type==GDK_TOUCH_CANCEL){
        v->scrubbing=FALSE;v->seek_target=gtk_range_get_value(GTK_RANGE(v->timeline));
        if(v->seek_timer){g_source_remove(v->seek_timer);v->seek_timer=0;}
        seek(v,v->seek_target,TRUE,TRUE);gtk_widget_grab_focus(GTK_WIDGET(v->area));
    }
    return FALSE;
}
static void toggle_play(Video *v) {
    if(!v->loaded)return;
    if(v->eof){seek(v,0,TRUE,TRUE);flag(v,"pause",FALSE);}else flag(v,"pause",!v->paused);
}
static void play_clicked(GtkButton *button,gpointer data){(void)button;toggle_play(data);}
static void mute_clicked(GtkButton *button,gpointer data){(void)button;Video*v=data;flag(v,"mute",!v->muted);}
static void fullscreen_clicked(GtkButton *button,gpointer data){(void)button;Video*v=data;zen_window_toggle_fullscreen(v->shell);}
static void next_subtitle(Video *v) {
    if(!v->loaded)return;
    if(!v->subtitle_count){choose_subtitles(v);return;}
    flag(v,"sub-visibility",TRUE);const char *args[]={"cycle","sid",NULL};command(v,args);
}
static void next_subtitle_clicked(GtkButton*b,gpointer data){(void)b;next_subtitle(data);}
static void open_subtitle_clicked(GtkButton*b,gpointer data){(void)b;choose_subtitles(data);}
static void subtitle_toggled(GtkCheckButton*b,gpointer data){Video*v=data;if(!v->updating)flag(v,"sub-visibility",gtk_check_button_get_active(b));}
static void add_subtitle(Video *v,const char *path) {
    if(!v->loaded){notice(v,"Open a video before adding subtitles.");return;}
    const char *args[]={"sub-add",path,"select",NULL};command(v,args);flag(v,"sub-visibility",TRUE);
    g_free(v->subtitle_path);v->subtitle_path=g_strdup(path);
    if(diagnostics)g_printerr("zen-video subtitle-added\n");
}
static void subtitle_response(GtkNativeDialog *dialog,int response,gpointer data) {
    Video *v=data;
    if(!g_atomic_int_get(&v->closing)&&response==GTK_RESPONSE_ACCEPT){
        GFile *file=gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dialog));
        char *path=file?g_file_get_path(file):NULL;
        if(path)add_subtitle(v,path);else notice(v,"Choose a local subtitle file.");
        g_free(path);g_clear_object(&file);
    }
    v->subtitle_dialog=NULL;gtk_native_dialog_destroy(dialog);g_object_unref(dialog);
}
static void subtitle_closure_unref(gpointer data,GClosure *closure){(void)closure;video_unref(data);}
static void choose_subtitles(Video *v) {
    if(!v->loaded||v->subtitle_dialog||g_atomic_int_get(&v->closing))return;
    GtkFileChooserNative *dialog=gtk_file_chooser_native_new("Open subtitles",v->window,GTK_FILE_CHOOSER_ACTION_OPEN,"Open","Cancel");
    GtkFileFilter *filter=gtk_file_filter_new();gtk_file_filter_set_name(filter,"Subtitle files");
    const char *patterns[]={"*.srt","*.vtt","*.ass","*.ssa","*.sub","*.SRT",NULL};
    for(int i=0;patterns[i];i++)gtk_file_filter_add_pattern(filter,patterns[i]);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog),filter);g_object_unref(filter);
    v->subtitle_dialog=dialog;
    g_signal_connect_data(dialog,"response",G_CALLBACK(subtitle_response),video_ref(v),subtitle_closure_unref,0);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
}
static mpv_node *node_get(mpv_node *node,const char *key) {
    if(!node||node->format!=MPV_FORMAT_NODE_MAP)return NULL;
    for(int i=0;i<node->u.list->num;i++)if(!strcmp(node->u.list->keys[i],key))return &node->u.list->values[i];
    return NULL;
}
static const char *node_string(mpv_node *node){return node&&node->format==MPV_FORMAT_STRING?node->u.string:NULL;}
static gboolean node_flag(mpv_node *node){return node&&node->format==MPV_FORMAT_FLAG&&node->u.flag;}
static void tracks_changed(Video *v,mpv_node *tracks) {
    v->has_video=v->has_audio=FALSE;v->subtitle_count=0;g_clear_pointer(&v->caption,g_free);
    if(tracks->format!=MPV_FORMAT_NODE_ARRAY)return;
    for(int i=0;i<tracks->u.list->num;i++){
        mpv_node *track=&tracks->u.list->values[i];const char *type=node_string(node_get(track,"type"));
        if(g_strcmp0(type,"video")==0&&!node_flag(node_get(track,"albumart")))v->has_video=TRUE;
        else if(g_strcmp0(type,"audio")==0)v->has_audio=TRUE;
        else if(g_strcmp0(type,"sub")==0){
            v->subtitle_count++;
            if(node_flag(node_get(track,"selected"))){
                const char *title=node_string(node_get(track,"title")),*language=node_string(node_get(track,"lang"));
                g_free(v->caption);
                v->caption=short_text(title&&*title?title:language&&*language?language:"Subtitles on");
            }
        }
    }
    if(diagnostics)g_printerr("zen-video tracks video=%d audio=%d subtitles=%d selected-subtitle=%d\n",v->has_video,v->has_audio,v->subtitle_count,v->caption!=NULL);
    gtk_gl_area_queue_render(v->area);
}
static void property_changed(Video *v,mpv_event_property *p) {
    if(!p->data)return;
    gboolean force=TRUE;
    if(p->format==MPV_FORMAT_DOUBLE){
        double value=*(double*)p->data;if(!isfinite(value))return;
        if(!strcmp(p->name,"time-pos")){
            v->position=MAX(0,value);force=FALSE;
            if(diagnostics&&(int)v->position!=v->logged_second){v->logged_second=(int)v->position;g_printerr("zen-video position=%.3f\n",v->position);}
        }else if(!strcmp(p->name,"duration"))v->duration=MAX(0,value);
        else if(!strcmp(p->name,"volume"))v->volume=CLAMP(value,0,100);
    }else if(p->format==MPV_FORMAT_FLAG){
        gboolean value=*(int*)p->data;
        if(!strcmp(p->name,"pause"))v->paused=value;
        else if(!strcmp(p->name,"mute"))v->muted=value;
        else if(!strcmp(p->name,"eof-reached"))v->eof=value;
        else if(!strcmp(p->name,"seekable"))v->seekable=value;
        else if(!strcmp(p->name,"sub-visibility"))v->sub_visible=value;
        if(diagnostics)g_printerr("zen-video %s=%d\n",p->name,value);
    }else if(p->format==MPV_FORMAT_STRING){
        const char *value=*(char**)p->data;
        if(!strcmp(p->name,"current-ao")){g_free(v->audio_output);v->audio_output=g_strdup(value);}
        if(diagnostics)g_printerr("zen-video %s=%s\n",p->name,value?value:"");
    }else if(p->format==MPV_FORMAT_NODE&&!strcmp(p->name,"track-list"))tracks_changed(v,p->data);
    update_ui(v,force);
}
static gboolean dispatch(gpointer data) {
    Video *v=data;g_atomic_int_set(&v->queued,0);
    if(g_atomic_int_get(&v->closing)||!v->mpv)return G_SOURCE_REMOVE;
    for(int count=0;;count++){
        mpv_event *event=mpv_wait_event(v->mpv,0);
        if(event->event_id==MPV_EVENT_NONE)break;
        if(event->event_id==MPV_EVENT_PROPERTY_CHANGE)property_changed(v,event->data);
        else if(event->event_id==MPV_EVENT_START_FILE)zen_window_show_progress(v->shell,-1.);
        else if(event->event_id==MPV_EVENT_FILE_LOADED){
            zen_window_hide_progress(v->shell);
            v->loaded=TRUE;zen_window_set_ready(v->shell,TRUE);zen_window_clear_notice(v->shell);
            if(v->recovering){
                seek(v,v->restore_position,TRUE,TRUE);flag(v,"pause",v->restore_pause);v->recovering=FALSE;
                if(v->subtitle_path){const char *args[]={"sub-add",v->subtitle_path,"select",NULL};command(v,args);}
            }
            if(diagnostics)g_printerr("zen-video loaded\n");
            update_ui(v,TRUE);gtk_gl_area_queue_render(v->area);
        }else if(event->event_id==MPV_EVENT_END_FILE){
            mpv_event_end_file *end=event->data;
            if(end->reason!=MPV_END_FILE_REASON_STOP&&end->reason!=MPV_END_FILE_REASON_REDIRECT)
                zen_window_hide_progress(v->shell);
            if(end->reason==MPV_END_FILE_REASON_ERROR){
                v->loaded=FALSE;zen_window_set_ready(v->shell,FALSE);
                char *message=g_strdup_printf("Could not play this file: %s",mpv_error_string(end->error));notice(v,message);g_free(message);
                if(diagnostics)g_printerr("zen-video playback-error=%s\n",mpv_error_string(end->error));
                update_ui(v,TRUE);gtk_gl_area_queue_render(v->area);
            }
        }else if(event->event_id==MPV_EVENT_COMMAND_REPLY&&event->error<0){
            if(event->reply_userdata==v->load_id)zen_window_hide_progress(v->shell);
            if(event->reply_userdata==0||event->reply_userdata==v->load_id)notice(v,mpv_error_string(event->error));
            if(diagnostics)g_printerr("zen-video command-error=%s\n",mpv_error_string(event->error));
        }else if(event->event_id==MPV_EVENT_LOG_MESSAGE&&diagnostics){
            mpv_event_log_message *message=event->data;g_printerr("mpv[%s] %s",message->prefix,message->text);
        }
        if(count>=255){wakeup(v);break;}
    }
    if(v->render&&v->gl_context){
        gdk_gl_context_make_current(v->gl_context);
        if(mpv_render_context_update(v->render)&MPV_RENDER_UPDATE_FRAME)gtk_gl_area_queue_render(v->area);
    }
    return G_SOURCE_REMOVE;
}
/* Always enqueue, even when called on GTK's thread. g_main_context_invoke may
 * run inline and would re-enter libmpv while one of its callbacks holds locks. */
static void wakeup(void *data) {
    Video *v=data;
    if(g_atomic_int_get(&v->closing)||!g_atomic_int_compare_and_exchange(&v->queued,0,1))return;
    GSource *source=g_idle_source_new();g_source_set_priority(source,G_PRIORITY_DEFAULT);
    g_source_set_callback(source,dispatch,video_ref(v),video_unref);g_source_attach(source,NULL);g_source_unref(source);
}
static gboolean init_player(Video *v) {
    v->mpv=mpv_create();if(!v->mpv){notice(v,"Could not start the video player.");return FALSE;}
    const char *options[][2]={{"config","no"},{"terminal","no"},{"vo","libmpv"},{"idle","yes"},
        {"keep-open","yes"},{"input-default-bindings","no"},{"input-vo-keyboard","no"},{"osc","no"},
        {"osd-level","0"},{"audio-display","no"},{"load-scripts","no"},{"hwdec","auto-safe"},
        {"access-references","no"},{"ytdl","no"},{"video-timing-offset","0"},{"sub-auto","exact"}};
    for(guint i=0;i<G_N_ELEMENTS(options);i++)mpv_set_option_string(v->mpv,options[i][0],options[i][1]);
    v->quiet_audio=g_getenv("ZEN_VIDEO_SILENT")!=NULL;
    mpv_set_option_string(v->mpv,"ao",v->quiet_audio?"null":"pulse,alsa,null");
    int error=mpv_initialize(v->mpv);
    if(error<0){notice(v,mpv_error_string(error));mpv_terminate_destroy(v->mpv);v->mpv=NULL;return FALSE;}
    const struct {const char *name;mpv_format format;} properties[]={
        {"time-pos",MPV_FORMAT_DOUBLE},{"duration",MPV_FORMAT_DOUBLE},{"volume",MPV_FORMAT_DOUBLE},
        {"pause",MPV_FORMAT_FLAG},{"mute",MPV_FORMAT_FLAG},{"eof-reached",MPV_FORMAT_FLAG},
        {"seekable",MPV_FORMAT_FLAG},{"sub-visibility",MPV_FORMAT_FLAG},{"track-list",MPV_FORMAT_NODE},
        {"current-ao",MPV_FORMAT_STRING},{"hwdec-current",MPV_FORMAT_STRING},{"video-codec",MPV_FORMAT_STRING}};
    for(guint i=0;i<G_N_ELEMENTS(properties);i++)mpv_observe_property(v->mpv,i+1,properties[i].name,properties[i].format);
    if(diagnostics)mpv_request_log_messages(v->mpv,"warn");
    mpv_set_wakeup_callback(v->mpv,wakeup,v);return TRUE;
}
static void *get_proc(void *data,const char *name) {
    (void)data;
    if(eglGetCurrentContext()!=EGL_NO_CONTEXT)return(void*)eglGetProcAddress(name);
#ifdef GDK_WINDOWING_X11
    return(void*)glXGetProcAddressARB((const GLubyte*)name);
#else
    return NULL;
#endif
}
static void realize(GtkGLArea *area,gpointer data) {
    Video *v=data;if(!v->mpv||g_atomic_int_get(&v->closing))return;
    gtk_gl_area_make_current(area);
    if(gtk_gl_area_get_error(area)){zen_window_hide_progress(v->shell);return;}
    v->gl_context=g_object_ref(gtk_gl_area_get_context(area));
    mpv_opengl_init_params init={.get_proc_address=get_proc};
    mpv_render_param params[5]={{MPV_RENDER_PARAM_API_TYPE,(void*)MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS,&init},{0,NULL},{0,NULL},{0,NULL}};
    GdkDisplay *display=gtk_widget_get_display(GTK_WIDGET(area));
#ifdef GDK_WINDOWING_X11
    if(GDK_IS_X11_DISPLAY(display))params[2]=(mpv_render_param){MPV_RENDER_PARAM_X11_DISPLAY,gdk_x11_display_get_xdisplay(display)};
#endif
#ifdef GDK_WINDOWING_WAYLAND
    if(GDK_IS_WAYLAND_DISPLAY(display))params[2]=(mpv_render_param){MPV_RENDER_PARAM_WL_DISPLAY,gdk_wayland_display_get_wl_display(display)};
#endif
    int error=mpv_render_context_create(&v->render,v->mpv,params);
    if(error<0){
        zen_window_hide_progress(v->shell);
        GError *problem=g_error_new_literal(G_IO_ERROR,G_IO_ERROR_FAILED,mpv_error_string(error));
        gtk_gl_area_set_error(area,problem);g_error_free(problem);g_clear_object(&v->gl_context);return;
    }
    if(diagnostics)g_printerr("zen-video renderer=%s\n",glGetString(GL_RENDERER));
    mpv_render_context_set_update_callback(v->render,wakeup,v);load_pending(v);
}
static void release_render(Video *v) {
    if(v->render){
        gdk_gl_context_make_current(v->gl_context);
        mpv_render_context_set_update_callback(v->render,NULL,NULL);mpv_render_context_free(v->render);v->render=NULL;
    }
    g_clear_object(&v->gl_context);
}
static void unrealize(GtkGLArea *area,gpointer data) {
    (void)area;Video *v=data;
    if(!g_atomic_int_get(&v->closing)&&v->path){v->recovering=TRUE;v->restore_position=v->position;v->restore_pause=v->paused;flag(v,"pause",TRUE);}
    release_render(v);
}
static gboolean render(GtkGLArea *area,GdkGLContext *context,gpointer data) {
    (void)context;Video *v=data;
    int scale=gtk_widget_get_scale_factor(GTK_WIDGET(area));
    int width=gtk_widget_get_width(GTK_WIDGET(area))*scale,height=gtk_widget_get_height(GTK_WIDGET(area))*scale;
    glViewport(0,0,width,height);glDisable(GL_SCISSOR_TEST);glClearColor(.063f,.078f,.071f,1);glClear(GL_COLOR_BUFFER_BIT);
    if(!v->render||!v->loaded||!v->has_video||g_atomic_int_get(&v->closing))return TRUE;
    GLint framebuffer=0,read_framebuffer=0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&framebuffer);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read_framebuffer);
    mpv_opengl_fbo fbo={framebuffer,width,height,0};int flip=1,block=0;
    mpv_render_param params[]={{MPV_RENDER_PARAM_OPENGL_FBO,&fbo},{MPV_RENDER_PARAM_FLIP_Y,&flip},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME,&block},{0,NULL}};
    int error=mpv_render_context_render(v->render,params);
    if(error<0&&diagnostics)g_printerr("zen-video render-error=%s\n",mpv_error_string(error));
    /* libmpv restores the default FBO; GtkGLArea still needs its own attachment
     * bound when it resolves/snapshots the result after this callback. */
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,framebuffer);glBindFramebuffer(GL_READ_FRAMEBUFFER,read_framebuffer);
    return TRUE;
}
static gboolean layout_controls(gpointer data) {
    Video *v=data;v->layout_idle=0;if(g_atomic_int_get(&v->closing))return G_SOURCE_REMOVE;
    int width=gtk_widget_get_width(GTK_WIDGET(v->area));
    gtk_widget_set_size_request(v->timeline,CLAMP(width-330,80,560),-1);
    gtk_widget_set_visible(v->subtitles,width>=500);gtk_widget_set_visible(v->fullscreen,width>=420);
    update_ui(v,TRUE);return G_SOURCE_REMOVE;
}
static void resize(GtkGLArea *area,int width,int height,gpointer data) {
    (void)width;(void)height;Video*v=data;if(!v->layout_idle)v->layout_idle=g_idle_add(layout_controls,v);gtk_gl_area_queue_render(area);
}
static gboolean subtitle_file(const char *path) {
    const char *ext=strrchr(path,'.');return ext&&(!g_ascii_strcasecmp(ext,".srt")||!g_ascii_strcasecmp(ext,".vtt")||!g_ascii_strcasecmp(ext,".ass")||!g_ascii_strcasecmp(ext,".ssa")||!g_ascii_strcasecmp(ext,".sub"));
}
static void load_pending(Video *v) {
    if(!v->mpv||!v->render||!v->path)return;
    const char *args[]={"loadfile",v->path,"replace",NULL};
    int error=mpv_command_async(v->mpv,++v->load_id,args);
    if(error<0){zen_window_hide_progress(v->shell);notice(v,mpv_error_string(error));}
    else {
        zen_window_clear_notice(v->shell);
        zen_window_show_progress(v->shell,-1.);
        if(!v->recovering)flag(v,"pause",FALSE);
    }
}
static void open_file(ZenWindow *shell,GFile *file,gpointer data) {
    (void)shell;Video *v=data;char *path=g_file_get_path(file);
    if(!path){notice(v,"Choose a local video file.");return;}
    if(subtitle_file(path)){add_subtitle(v,path);g_free(path);return;}
    if(v->seek_timer){g_source_remove(v->seek_timer);v->seek_timer=0;}
    g_free(v->path);v->path=path;g_clear_pointer(&v->subtitle_path,g_free);g_clear_pointer(&v->caption,g_free);
    v->loaded=v->eof=v->recovering=v->scrubbing=v->audio_warned=FALSE;v->position=v->duration=0;v->logged_second=-1;
    char *name=g_filename_display_basename(path);zen_window_set_title(v->shell,name);g_free(name);
    zen_window_set_ready(v->shell,TRUE);update_ui(v,TRUE);gtk_gl_area_queue_render(v->area);load_pending(v);
}
static gboolean key_pressed(GtkEventControllerKey *controller,guint key,guint code,GdkModifierType state,gpointer data) {
    (void)controller;(void)code;Video*v=data;gboolean ctrl=state&GDK_CONTROL_MASK;
    if(ctrl&&(key==GDK_KEY_o||key==GDK_KEY_O||key==GDK_KEY_w||key==GDK_KEY_W||key==GDK_KEY_q||key==GDK_KEY_Q))return FALSE;
    GtkWidget *focus=gtk_window_get_focus(v->window);if(focus&&GTK_IS_EDITABLE(focus))return FALSE;
    if(ctrl&&(state&GDK_SHIFT_MASK)&&(key==GDK_KEY_s||key==GDK_KEY_S)){choose_subtitles(v);return TRUE;}
    switch(key){
    case GDK_KEY_space:case GDK_KEY_k:toggle_play(v);return TRUE;
    case GDK_KEY_Left:seek(v,state&GDK_SHIFT_MASK?-30:-5,FALSE,FALSE);return TRUE;
    case GDK_KEY_Right:seek(v,state&GDK_SHIFT_MASK?30:5,FALSE,FALSE);return TRUE;
    case GDK_KEY_Up:number(v,"volume",MIN(100,v->volume+5));return TRUE;
    case GDK_KEY_Down:number(v,"volume",MAX(0,v->volume-5));return TRUE;
    case GDK_KEY_Home:seek(v,0,TRUE,TRUE);return TRUE;
    case GDK_KEY_m:case GDK_KEY_M:flag(v,"mute",!v->muted);return TRUE;
    case GDK_KEY_period:case GDK_KEY_greater:{const char *args[]={"frame-step",NULL};command(v,args);return TRUE;}
    case GDK_KEY_comma:case GDK_KEY_less:{const char *args[]={"frame-back-step",NULL};command(v,args);return TRUE;}
    case GDK_KEY_j:case GDK_KEY_J:next_subtitle(v);return TRUE;
    case GDK_KEY_v:case GDK_KEY_V:flag(v,"sub-visibility",!v->sub_visible);return TRUE;
    case GDK_KEY_f:zen_window_toggle_fullscreen(v->shell);return TRUE;
    case GDK_KEY_Escape:if(gtk_window_is_fullscreen(v->window))gtk_window_unfullscreen(v->window);gtk_widget_grab_focus(GTK_WIDGET(v->area));return TRUE;
    default:return FALSE;
    }
}
static void canvas_pressed(GtkGestureClick *gesture,int count,double x,double y,gpointer data) {
    (void)gesture;Video*v=data;
    if(y<72||zen_window_edge_at(v->shell,x,y)>=0)return;
    gtk_widget_grab_focus(GTK_WIDGET(v->area));
    if(count==2)zen_window_toggle_fullscreen(v->shell);else zen_window_reveal_controls(v->shell);
}
static void shutdown_player(Video *v) {
    if(!g_atomic_int_compare_and_exchange(&v->closing,0,1))return;
    if(v->seek_timer){g_source_remove(v->seek_timer);v->seek_timer=0;}
    if(v->layout_idle){g_source_remove(v->layout_idle);v->layout_idle=0;}
    if(v->subtitle_dialog){GtkFileChooserNative *dialog=v->subtitle_dialog;v->subtitle_dialog=NULL;gtk_native_dialog_destroy(GTK_NATIVE_DIALOG(dialog));g_object_unref(dialog);}
    release_render(v);
    if(v->mpv){mpv_set_wakeup_callback(v->mpv,NULL,NULL);mpv_terminate_destroy(v->mpv);v->mpv=NULL;}
}
static gboolean close_request(GtkWindow *window,gpointer data){(void)window;shutdown_player(data);return FALSE;}
static void window_data_free(gpointer data){Video*v=data;shutdown_player(v);video_unref(v);}
static void make_subtitle_menu(Video *v,GtkWidget *controls) {
    v->subtitles=gtk_menu_button_new();gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(v->subtitles),"media-view-subtitles-symbolic");
    gtk_widget_set_tooltip_text(v->subtitles,"Subtitles (J to cycle · Ctrl+Shift+S to open)");
    GtkWidget *popover=gtk_popover_new(),*box=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);
    gtk_widget_set_margin_top(box,12);gtk_widget_set_margin_bottom(box,12);gtk_widget_set_margin_start(box,12);gtk_widget_set_margin_end(box,12);
    v->subtitle_name=gtk_label_new("No subtitle selected");gtk_label_set_max_width_chars(GTK_LABEL(v->subtitle_name),32);gtk_label_set_ellipsize(GTK_LABEL(v->subtitle_name),PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box),v->subtitle_name);
    v->subtitle_toggle=gtk_check_button_new_with_label("Show subtitles");g_signal_connect(v->subtitle_toggle,"toggled",G_CALLBACK(subtitle_toggled),v);gtk_box_append(GTK_BOX(box),v->subtitle_toggle);
    GtkWidget *next=gtk_button_new_with_label("Next subtitle track"),*open=gtk_button_new_with_label("Open subtitle file…");
    g_signal_connect(next,"clicked",G_CALLBACK(next_subtitle_clicked),v);g_signal_connect(open,"clicked",G_CALLBACK(open_subtitle_clicked),v);
    gtk_box_append(GTK_BOX(box),next);gtk_box_append(GTK_BOX(box),open);gtk_popover_set_child(GTK_POPOVER(popover),box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(v->subtitles),popover);gtk_box_append(GTK_BOX(controls),v->subtitles);
}
static Video *new_window(GtkApplication *application) {
    Video *v=g_new0(Video,1);v->refs=1;v->volume=100;v->sub_visible=TRUE;v->logged_second=-1;
    v->shell=zen_window_new(application,"Zen Video",960,600);v->window=zen_window_get_window(v->shell);
    g_object_set_data_full(G_OBJECT(v->window),"zen-video",v,window_data_free);g_signal_connect(v->window,"close-request",G_CALLBACK(close_request),v);
    zen_window_set_empty_text(v->shell,"Drop into the moment","Open a video, or drop one here.");zen_window_set_open_handler(v->shell,open_file,v);
    const char *mimes[]={"video/*","audio/*",NULL};const char *patterns[]={"*.mp4","*.mkv","*.webm","*.mov","*.avi","*.mpeg","*.mpg","*.m4v","*.ogv","*.ts","*.MP4","*.MKV",NULL};
    zen_window_add_file_filter(v->shell,"Videos",mimes,patterns);
    GtkWidget *overlay=gtk_overlay_new();v->area=GTK_GL_AREA(gtk_gl_area_new());
    gtk_gl_area_set_required_version(v->area,3,3);gtk_gl_area_set_use_es(v->area,FALSE);gtk_gl_area_set_auto_render(v->area,FALSE);
    gtk_widget_set_focusable(GTK_WIDGET(v->area),TRUE);gtk_widget_set_hexpand(GTK_WIDGET(v->area),TRUE);gtk_widget_set_vexpand(GTK_WIDGET(v->area),TRUE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(v->area),GTK_ACCESSIBLE_PROPERTY_LABEL,"Video. Space plays or pauses; arrow keys seek and change volume.",-1);
    gtk_overlay_set_child(GTK_OVERLAY(overlay),GTK_WIDGET(v->area));v->audio_label=gtk_label_new("Audio playback");gtk_widget_add_css_class(v->audio_label,"title-1");
    gtk_widget_set_can_target(v->audio_label,FALSE);gtk_widget_set_halign(v->audio_label,GTK_ALIGN_CENTER);gtk_widget_set_valign(v->audio_label,GTK_ALIGN_CENTER);gtk_widget_set_visible(v->audio_label,FALSE);gtk_overlay_add_overlay(GTK_OVERLAY(overlay),v->audio_label);
    zen_window_set_child(v->shell,overlay);
    GtkWidget *controls=zen_window_get_controls(v->shell);
    v->play=zen_button_new("media-playback-start-symbolic","Play or pause (Space)",G_CALLBACK(play_clicked),v);gtk_box_append(GTK_BOX(controls),v->play);
    v->timeline=gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,0,1,.1);gtk_scale_set_draw_value(GTK_SCALE(v->timeline),FALSE);gtk_range_set_increments(GTK_RANGE(v->timeline),1,10);
    gtk_widget_set_size_request(v->timeline,400,-1);gtk_widget_set_tooltip_text(v->timeline,"Playback position");gtk_accessible_update_property(GTK_ACCESSIBLE(v->timeline),GTK_ACCESSIBLE_PROPERTY_LABEL,"Playback position",-1);
    g_signal_connect(v->timeline,"value-changed",G_CALLBACK(seek_changed),v);gtk_box_append(GTK_BOX(controls),v->timeline);
    GtkEventController *seek_controller=gtk_event_controller_legacy_new();gtk_event_controller_set_propagation_phase(seek_controller,GTK_PHASE_CAPTURE);g_signal_connect(seek_controller,"event",G_CALLBACK(seek_event),v);gtk_widget_add_controller(v->timeline,seek_controller);
    v->time=gtk_label_new("0:00 / –:––");gtk_widget_add_css_class(v->time,"muted");gtk_box_append(GTK_BOX(controls),v->time);
    v->mute=zen_button_new("audio-volume-high-symbolic","Mute (M)",G_CALLBACK(mute_clicked),v);gtk_box_append(GTK_BOX(controls),v->mute);
    make_subtitle_menu(v,controls);
    v->fullscreen=zen_button_new("view-fullscreen-symbolic","Fullscreen (F or F11)",G_CALLBACK(fullscreen_clicked),v);gtk_box_append(GTK_BOX(controls),v->fullscreen);
    g_signal_connect(v->area,"realize",G_CALLBACK(realize),v);g_signal_connect(v->area,"unrealize",G_CALLBACK(unrealize),v);g_signal_connect(v->area,"render",G_CALLBACK(render),v);g_signal_connect(v->area,"resize",G_CALLBACK(resize),v);
    GtkEventController *keys=gtk_event_controller_key_new();gtk_event_controller_set_propagation_phase(keys,GTK_PHASE_CAPTURE);g_signal_connect(keys,"key-pressed",G_CALLBACK(key_pressed),v);gtk_widget_add_controller(GTK_WIDGET(v->window),keys);
    GtkGesture *click=gtk_gesture_click_new();gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),1);g_signal_connect(click,"pressed",G_CALLBACK(canvas_pressed),v);gtk_widget_add_controller(GTK_WIDGET(v->area),GTK_EVENT_CONTROLLER(click));
    init_player(v);update_ui(v,TRUE);zen_window_present(v->shell);gtk_widget_grab_focus(GTK_WIDGET(v->area));return v;
}
static void startup(GtkApplication *application,gpointer data){(void)application;(void)data;zen_ui_init();}
static void activate(GtkApplication *application,gpointer data){(void)data;new_window(application);}
static void app_open(GtkApplication *application,GFile **files,int count,const char *hint,gpointer data) {
    (void)hint;(void)data;for(int i=0;i<count;i++){Video*v=new_window(application);open_file(v->shell,files[i],v);}
}
int main(int argc,char **argv) {
    for(int i=1;i<argc;i++)if(!strcmp(argv[i],"--diagnostics")){diagnostics=TRUE;memmove(argv+i,argv+i+1,(gsize)(argc-i)*sizeof(char*));argc--;i--;}
    if(argc>1&&!strcmp(argv[1],"--version")){g_print("zen-video " ZEN_VERSION "\n");return 0;}
    if(argc>1&&!strcmp(argv[1],"--help")){
        g_print("zen-video " ZEN_VERSION "\nUsage: zen-video [--diagnostics] [FILE …]\n\nSpace play/pause · Left/Right seek 5s · Shift+Left/Right seek 30s\nUp/Down volume · M mute · . / , step forward/back a frame\nJ cycle subtitles · V show/hide · Ctrl+Shift+S open subtitles\nF / F11 fullscreen · Ctrl+O open · Ctrl+W close\n");return 0;
    }
    g_set_prgname("zen-video");g_set_application_name("Zen Video");
    GtkApplication *application=gtk_application_new("io.github.zenviewers.Video",G_APPLICATION_HANDLES_OPEN|G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application,"startup",G_CALLBACK(startup),NULL);g_signal_connect(application,"activate",G_CALLBACK(activate),NULL);g_signal_connect(application,"open",G_CALLBACK(app_open),NULL);
    int status=g_application_run(G_APPLICATION(application),argc,argv);g_object_unref(application);return status;
}
