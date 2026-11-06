/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "renderer.h"
#include <epoxy/gl.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#define UPLOAD_BYTES ((gsize)8*1024*1024)
#define GEOMETRY_LIMIT ((gsize)768*1024*1024)
#define TEXTURE_LIMIT ((gsize)256*1024*1024)
#define PI_F 3.14159265358979323846f

typedef struct {
    GLuint name;
    int width, height, channels, rows;
    gboolean ready;
    guint owner; /* Materials may share one decoded image and one GPU texture. */
} Texture;
typedef struct {
    guint material;
    GLenum primitive;
    GArray *counts, *offsets;
} Batch;
typedef struct {
    GLuint name;
    GLint mvp, eye, color, textured, primitive, wire, extent, floor, grid_scale;
} Program;
struct Zen3dRenderer {
    Zen3dModel *model;
    gboolean initialized, dirty, pending, cpu_released, needs_reload, failed;
    GLuint vao, vbo, ibo, empty_vao;
    Program mesh, ground, background;
    Texture *textures;
    guint texture_count, texture_cursor;
    gsize vertex_bytes, index_bytes, preview_bytes;
    gsize vertex_sent, index_sent, preview_sent, total_bytes, sent_bytes;
    GPtrArray *batches;
    /* Small uploaded scenes survive replacement until its first mesh is ready.
     * These records borrow the parent's programs; only geometry is owned. */
    struct Zen3dRenderer *previous, *retired;
};

static GQuark renderer_error(void) { return g_quark_from_static_string("zen3d-renderer"); }
static gboolean fail(GError **error,const char *message) {
    g_set_error_literal(error,renderer_error(),1,message); return FALSE;
}
static gboolean gl_check(GError **error,const char *operation) {
    GLenum e=glGetError();
    if (e==GL_NO_ERROR) return TRUE;
    const char *detail=e==GL_OUT_OF_MEMORY ? "GPU memory allocation failed" :
        e==GL_INVALID_FRAMEBUFFER_OPERATION ? "the drawing framebuffer is unavailable" : "the graphics driver rejected a command";
    g_set_error(error,renderer_error(),(int)e,"%s: %s (0x%x).",operation,detail,e);
    while (glGetError()!=GL_NO_ERROR) {}
    return FALSE;
}
static GLuint compile_shader(GLenum kind,const char *source,GError **error) {
    GLuint shader=glCreateShader(kind);
    glShaderSource(shader,1,&source,NULL); glCompileShader(shader);
    GLint okay=0; glGetShaderiv(shader,GL_COMPILE_STATUS,&okay);
    if (!okay) {
        char message[4096]; GLsizei n=0;
        glGetShaderInfoLog(shader,sizeof message-1,&n,message); message[n]=0;
        g_set_error(error,renderer_error(),2,"Could not compile the 3D shader: %s",message);
        glDeleteShader(shader); return 0;
    }
    return shader;
}
static gboolean make_program(Program *program,const char *vertex,const char *fragment,GError **error) {
    GLuint vs=compile_shader(GL_VERTEX_SHADER,vertex,error);
    if (!vs) return FALSE;
    GLuint fs=compile_shader(GL_FRAGMENT_SHADER,fragment,error);
    if (!fs) { glDeleteShader(vs); return FALSE; }
    GLuint name=glCreateProgram(); glAttachShader(name,vs); glAttachShader(name,fs);
    glLinkProgram(name); glDeleteShader(vs); glDeleteShader(fs);
    GLint okay=0; glGetProgramiv(name,GL_LINK_STATUS,&okay);
    if (!okay) {
        char message[4096]; GLsizei n=0;
        glGetProgramInfoLog(name,sizeof message-1,&n,message); message[n]=0;
        g_set_error(error,renderer_error(),3,"Could not link the 3D shader: %s",message);
        glDeleteProgram(name); return FALSE;
    }
    program->name=name;
#define LOCATION(field, uniform) program->field=glGetUniformLocation(name,uniform)
    LOCATION(mvp,"u_mvp"); LOCATION(eye,"u_eye"); LOCATION(color,"u_color");
    LOCATION(textured,"u_textured"); LOCATION(primitive,"u_primitive"); LOCATION(wire,"u_wire");
    LOCATION(extent,"u_extent"); LOCATION(floor,"u_floor"); LOCATION(grid_scale,"u_grid_scale");
#undef LOCATION
    return TRUE;
}

static const char *mesh_vertex=
    "#version 330 core\n"
    "layout(location=0) in vec3 a_position; layout(location=1) in vec3 a_normal;\n"
    "layout(location=2) in vec2 a_uv; layout(location=3) in vec4 a_color;\n"
    "uniform mat4 u_mvp; out vec3 v_position; out vec3 v_normal; out vec2 v_uv; out vec4 v_color;\n"
    "void main(){v_position=a_position;v_normal=a_normal;v_uv=a_uv;v_color=a_color;"
    "gl_Position=u_mvp*vec4(a_position,1);gl_PointSize=4.0;}\n";
static const char *mesh_fragment=
    "#version 330 core\n"
    "in vec3 v_position; in vec3 v_normal; in vec2 v_uv; in vec4 v_color;\n"
    "uniform vec3 u_eye; uniform vec4 u_color; uniform bool u_textured; uniform int u_primitive;"
    "uniform bool u_wire; uniform sampler2D u_texture; out vec4 out_color;\n"
    "void main(){"
    "if(u_wire){out_color=vec4(.075,.13,.17,.52);return;}"
    "vec4 c=u_color*v_color; vec3 base=pow(clamp(c.rgb,0.0,1.0),vec3(2.2));"
    "if(u_textured){vec4 tex=texture(u_texture,v_uv);base*=tex.rgb;c.a*=tex.a;}"
    "if(c.a<.04)discard;"
    "if(u_primitive==2){float d=length(gl_PointCoord-vec2(.5));if(d>.5)discard;"
    "c.a*=1.0-smoothstep(.38,.5,d);}"
    "if(u_primitive!=0){out_color=vec4(pow(base,vec3(1.0/2.2)),c.a);return;}"
    "vec3 n=length(v_normal)>.001?normalize(v_normal):normalize(cross(dFdx(v_position),dFdy(v_position)));"
    "if(!gl_FrontFacing)n=-n;"
    "vec3 light=normalize(vec3(-.45,.85,.55));vec3 fill=normalize(vec3(.75,.25,-.55));"
    "float key=max(dot(n,light),0.0);float bounce=max(dot(n,fill),0.0);"
    "vec3 eye=normalize(u_eye-v_position);vec3 half_direction=normalize(eye+light);"
    "float spec=pow(max(dot(n,half_direction),0.0),48.0)*key*.16;"
    "float ambient=.23+.12*(n.y*.5+.5);"
    "vec3 lit=base*(ambient+.69*key+.23*bounce)+vec3(spec);"
    "out_color=vec4(pow(clamp(lit,0.0,1.0),vec3(1.0/2.2)),c.a);}\n";
static const char *background_vertex=
    "#version 330 core\n"
    "out vec2 v_uv;void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
    "v_uv=p;gl_Position=vec4(p*2.0-1.0,.9999,1);}\n";
static const char *background_fragment=
    "#version 330 core\n"
    "in vec2 v_uv;out vec4 out_color;void main(){"
    "vec3 low=vec3(.84,.875,.87),high=vec3(.955,.967,.95);"
    "vec3 c=mix(low,high,smoothstep(0.0,1.0,v_uv.y));"
    "float vignette=dot(v_uv-vec2(.5),v_uv-vec2(.5))*.025;"
    "out_color=vec4(c-vignette,1);}\n";
static const char *ground_vertex=
    "#version 330 core\n"
    "uniform mat4 u_mvp;uniform float u_floor;out vec2 v_ground;"
    "void main(){vec2 p[6]=vec2[6](vec2(-1,-1),vec2(1,-1),vec2(1,1),vec2(-1,-1),vec2(1,1),vec2(-1,1));"
    "v_ground=p[gl_VertexID]*16.0;gl_Position=u_mvp*vec4(v_ground.x,u_floor,v_ground.y,1);}\n";
static const char *ground_fragment=
    "#version 330 core\n"
    "in vec2 v_ground;uniform vec3 u_extent;uniform float u_grid_scale;out vec4 out_color;"
    "void main(){vec2 g=v_ground/u_grid_scale;vec2 derivative=max(fwidth(g),vec2(.00001));"
    "vec2 thin=abs(fract(g-.5)-.5)/derivative;float line=1.0-min(min(thin.x,thin.y),1.0);"
    "vec2 major=abs(fract(g/5.0-.5)-.5)/max(fwidth(g/5.0),vec2(.00001));"
    "float strong=1.0-min(min(major.x,major.y),1.0);"
    "float fade=exp(-dot(v_ground,v_ground)*.08);"
    "vec2 radius=max(u_extent.xz,vec2(.15));float shadow=exp(-dot(v_ground/radius,v_ground/radius)*1.8)*.18;"
    "float a=fade*(line*.085+strong*.065)+shadow;"
    "out_color=vec4(.25,.35,.34,clamp(a,0.0,.3));}\n";

static void batch_free(gpointer data) {
    Batch *batch=data; g_array_unref(batch->counts); g_array_unref(batch->offsets); g_free(batch);
}
static void clear_geometry(Zen3dRenderer *r) {
    if (r->vao) glDeleteVertexArrays(1,&r->vao);
    if (r->vbo) glDeleteBuffers(1,&r->vbo);
    if (r->ibo) glDeleteBuffers(1,&r->ibo);
    r->vao=r->vbo=r->ibo=0;
    for (guint i=0;i<r->texture_count;i++) if (r->textures[i].name) glDeleteTextures(1,&r->textures[i].name);
    g_clear_pointer(&r->textures,g_free); r->texture_count=r->texture_cursor=0;
    g_clear_pointer(&r->batches,g_ptr_array_unref);
    r->vertex_bytes=r->index_bytes=r->preview_bytes=0;
    r->vertex_sent=r->index_sent=r->preview_sent=r->sent_bytes=r->total_bytes=0;
}
static void destroy_previous_gl(Zen3dRenderer *scene) {
    if (!scene) return;
    clear_geometry(scene); zen3d_model_free(scene->model); g_free(scene);
}
static void retire_previous(Zen3dRenderer *r) {
    if (!r->previous) return;
    r->previous->retired=r->retired; r->retired=r->previous; r->previous=NULL;
}
static void drain_retired(Zen3dRenderer *r) {
    while (r->retired) {
        Zen3dRenderer *scene=r->retired; r->retired=scene->retired;
        scene->retired=NULL; destroy_previous_gl(scene);
    }
}
static void retain_small_scene(Zen3dRenderer *r) {
    retire_previous(r);
    Zen3dRenderer *scene=g_memdup2(r,sizeof *r);
    scene->previous=scene->retired=NULL;
    r->previous=scene; r->model=NULL;
    r->vao=r->vbo=r->ibo=0; r->textures=NULL; r->texture_count=r->texture_cursor=0;
    r->batches=NULL; r->vertex_bytes=r->index_bytes=r->preview_bytes=0;
    r->vertex_sent=r->index_sent=r->preview_sent=r->total_bytes=r->sent_bytes=0;
}
Zen3dRenderer *zen3d_renderer_new(void) { return g_new0(Zen3dRenderer,1); }
gboolean zen3d_renderer_initialize(Zen3dRenderer *r,GError **error) {
    g_return_val_if_fail(r!=NULL,FALSE);
    if (r->initialized) return TRUE;
    if (!epoxy_is_desktop_gl() || epoxy_gl_version()<33)
        return fail(error,"Zen 3D needs desktop OpenGL 3.3 or newer.");
    /* Discard errors from the caller's context setup before checking our work. */
    while (glGetError()!=GL_NO_ERROR) {}
    if (!make_program(&r->mesh,mesh_vertex,mesh_fragment,error) ||
        !make_program(&r->background,background_vertex,background_fragment,error) ||
        !make_program(&r->ground,ground_vertex,ground_fragment,error)) {
        zen3d_renderer_release_gl(r); return FALSE;
    }
    glGenVertexArrays(1,&r->empty_vao);
    if (!gl_check(error,"Creating the renderer")) { zen3d_renderer_release_gl(r); return FALSE; }
    r->initialized=TRUE; r->dirty=r->model && !r->needs_reload; r->failed=FALSE;
    r->pending=r->dirty; return TRUE;
}
void zen3d_renderer_set_model(Zen3dRenderer *r,Zen3dModel *model) {
    g_return_if_fail(r!=NULL);
    if (r->model==model) return;
    if (model && r->model && r->initialized && !r->pending && !r->failed && r->vao &&
        r->total_bytes<=32u*1024*1024)
        retain_small_scene(r);
    else if (!model) retire_previous(r);
    zen3d_model_free(r->model); r->model=model;
    r->cpu_released=r->needs_reload=r->failed=FALSE;
    r->dirty=TRUE; r->pending=model!=NULL;
}
const Zen3dModel *zen3d_renderer_get_model(const Zen3dRenderer *r) { return r?r->model:NULL; }
gboolean zen3d_renderer_pending(const Zen3dRenderer *r) { return r && r->pending && !r->failed && !r->needs_reload; }
double zen3d_renderer_progress(const Zen3dRenderer *r) {
    if (!r || !r->model) return 1;
    if (r->dirty || r->needs_reload) return 0;
    return r->total_bytes ? CLAMP((double)r->sent_bytes/r->total_bytes,0,1) : 1;
}
gboolean zen3d_renderer_needs_reload(const Zen3dRenderer *r) { return r && r->needs_reload; }
void zen3d_renderer_release_gl(Zen3dRenderer *r) {
    if (!r) return;
    drain_retired(r); destroy_previous_gl(r->previous); r->previous=NULL;
    clear_geometry(r);
    if (r->mesh.name) glDeleteProgram(r->mesh.name);
    if (r->background.name) glDeleteProgram(r->background.name);
    if (r->ground.name) glDeleteProgram(r->ground.name);
    if (r->empty_vao) glDeleteVertexArrays(1,&r->empty_vao);
    memset(&r->mesh,0,sizeof r->mesh); memset(&r->background,0,sizeof r->background); memset(&r->ground,0,sizeof r->ground);
    r->empty_vao=0; r->initialized=FALSE; r->needs_reload=r->model && r->cpu_released;
    r->pending=r->dirty=r->model && !r->needs_reload;
}
void zen3d_renderer_free(Zen3dRenderer *r) {
    if (!r) return;
    /* GL deletion belongs to release_gl while GtkGLArea still has its context. */
    zen3d_renderer_free(r->previous);
    while (r->retired) {
        Zen3dRenderer *scene=r->retired; r->retired=scene->retired;
        scene->retired=NULL; zen3d_renderer_free(scene);
    }
    g_clear_pointer(&r->textures,g_free); g_clear_pointer(&r->batches,g_ptr_array_unref);
    zen3d_model_free(r->model); g_free(r);
}

static gboolean prepare_geometry(Zen3dRenderer *r,GError **error) {
    clear_geometry(r); r->dirty=FALSE;
    Zen3dModel *m=r->model;
    if (!m) { r->pending=FALSE; return TRUE; }
    if (!m->vertices || !m->indices || !m->vertex_count || !m->index_count || !m->parts || !m->part_count)
        return fail(error,"The model has no usable indexed geometry.");
    if (m->part_count>1000000) return fail(error,"The model contains more than one million geometry parts.");
    if (m->material_count>65536 || (m->material_count && !m->materials))
        return fail(error,"The model contains too many materials or invalid material data.");
    r->vertex_bytes=(gsize)m->vertex_count*sizeof(Zen3dVertex);
    r->index_bytes=(gsize)m->index_count*sizeof(uint32_t);
    r->preview_bytes=(gsize)m->preview_index_count*sizeof(uint32_t);
    if (r->preview_bytes && (!m->preview_indices || m->part_count!=1 ||
        m->parts[0].primitive!=ZEN3D_TRIANGLES || m->preview_index_count%3 ||
        m->preview_index_count>m->parts[0].index_count))
        return fail(error,"The model contains invalid motion-preview geometry.");
    if (r->vertex_bytes>GEOMETRY_LIMIT || r->index_bytes>GEOMETRY_LIMIT-r->vertex_bytes ||
        r->preview_bytes>GEOMETRY_LIMIT-r->vertex_bytes-r->index_bytes)
        return fail(error,"The model exceeds the 768 MiB GPU geometry limit.");
    r->total_bytes=r->vertex_bytes+r->index_bytes+r->preview_bytes;
    GLint max_texture=0; glGetIntegerv(GL_MAX_TEXTURE_SIZE,&max_texture);
    r->texture_count=m->material_count; r->textures=g_new0(Texture,r->texture_count);
    gsize texture_bytes=0;
    GHashTable *images=g_hash_table_new(g_direct_hash,g_direct_equal);
    for (guint i=0;i<m->material_count;i++) {
        GdkPixbuf *image=m->materials[i].texture;
        Texture *t=&r->textures[i]; t->owner=i;
        if (!image) continue;
        gpointer shared=g_hash_table_lookup(images,image);
        if (shared) { t->owner=GPOINTER_TO_UINT(shared)-1; continue; }
        g_hash_table_insert(images,image,GUINT_TO_POINTER(i+1));
        t->width=gdk_pixbuf_get_width(image); t->height=gdk_pixbuf_get_height(image);
        t->channels=gdk_pixbuf_get_n_channels(image);
        if (t->width<1 || t->height<1 || t->width>max_texture || t->height>max_texture ||
            (t->channels!=3 && t->channels!=4) || gdk_pixbuf_get_bits_per_sample(image)!=8) {
            g_hash_table_unref(images); return fail(error,"A model texture exceeds the graphics driver's image limits.");
        }
        gsize bytes=(gsize)t->width*t->height*4;
        /* Include approximately 1/3 extra for mipmaps. */
        if (bytes>TEXTURE_LIMIT || texture_bytes>TEXTURE_LIMIT-bytes) {
            g_hash_table_unref(images); return fail(error,"The model exceeds the 256 MiB GPU texture budget.");
        }
        texture_bytes+=bytes; r->total_bytes+=(gsize)t->width*t->height*t->channels;
    }
    g_hash_table_unref(images);
    if (texture_bytes>TEXTURE_LIMIT*3/4)
        return fail(error,"Texture mipmaps would exceed the 256 MiB GPU texture budget.");
    r->batches=g_ptr_array_new_with_free_func(batch_free);
    GHashTable *lookup=g_hash_table_new(g_direct_hash,g_direct_equal);
    for (guint i=0;i<m->part_count;i++) {
        Zen3dPart *part=&m->parts[i];
        if ((guint64)part->first_index+part->index_count>m->index_count || part->primitive<ZEN3D_TRIANGLES || part->primitive>ZEN3D_POINTS) {
            g_hash_table_unref(lookup); return fail(error,"The model contains an invalid geometry range.");
        }
        if (!part->index_count) continue;
        guint material=part->material<m->material_count?part->material:m->material_count;
        gpointer key=GUINT_TO_POINTER(material*3+part->primitive+1);
        Batch *batch=g_hash_table_lookup(lookup,key);
        if (!batch) {
            batch=g_new0(Batch,1); batch->material=material;
            batch->primitive=part->primitive==ZEN3D_TRIANGLES?GL_TRIANGLES:part->primitive==ZEN3D_LINES?GL_LINES:GL_POINTS;
            batch->counts=g_array_new(FALSE,FALSE,sizeof(GLsizei)); batch->offsets=g_array_new(FALSE,FALSE,sizeof(gpointer));
            g_hash_table_insert(lookup,key,batch); g_ptr_array_add(r->batches,batch);
        }
        guint remaining=part->index_count, first=part->first_index;
        while (remaining) {
            GLsizei count=(GLsizei)MIN(remaining,2147483646u);
            gpointer offset=(gpointer)(uintptr_t)((gsize)first*sizeof(uint32_t));
            g_array_append_val(batch->counts,count); g_array_append_val(batch->offsets,offset);
            first+=(guint)count; remaining-=(guint)count;
        }
    }
    g_hash_table_unref(lookup);
    glGenVertexArrays(1,&r->vao); glBindVertexArray(r->vao);
    glGenBuffers(1,&r->vbo); glBindBuffer(GL_ARRAY_BUFFER,r->vbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)r->vertex_bytes,NULL,GL_STATIC_DRAW);
    glGenBuffers(1,&r->ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,r->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)(r->index_bytes+r->preview_bytes),NULL,GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(Zen3dVertex),(void*)offsetof(Zen3dVertex,position));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(Zen3dVertex),(void*)offsetof(Zen3dVertex,normal));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,sizeof(Zen3dVertex),(void*)offsetof(Zen3dVertex,uv));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3,4,GL_UNSIGNED_BYTE,GL_TRUE,sizeof(Zen3dVertex),(void*)offsetof(Zen3dVertex,color));
    glBindVertexArray(0); glBindBuffer(GL_ARRAY_BUFFER,0);
    return gl_check(error,"Allocating model geometry");
}
static gboolean upload_chunk(Zen3dRenderer *r,GError **error) {
    if (!r->pending || !r->model) return TRUE;
    if (r->dirty && !prepare_geometry(r,error)) return FALSE;
    Zen3dModel *m=r->model;
    gint64 until=g_get_monotonic_time()+4000;
    gsize available=UPLOAD_BYTES;
    glBindVertexArray(r->vao); glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS,0); glPixelStorei(GL_UNPACK_SKIP_ROWS,0);
    while (available && g_get_monotonic_time()<until) {
        if (r->vertex_sent<r->vertex_bytes || r->preview_sent<r->preview_bytes || r->index_sent<r->index_bytes) {
            gboolean vertex=r->vertex_sent<r->vertex_bytes;
            gboolean preview=!vertex && r->preview_sent<r->preview_bytes;
            gsize *sent=vertex?&r->vertex_sent:preview?&r->preview_sent:&r->index_sent;
            gsize total=vertex?r->vertex_bytes:preview?r->preview_bytes:r->index_bytes;
            gsize bytes=MIN(MIN(total-*sent,available),(gsize)2*1024*1024);
            GLenum target=vertex?GL_ARRAY_BUFFER:GL_ELEMENT_ARRAY_BUFFER;
            glBindBuffer(target,vertex?r->vbo:r->ibo);
            const guchar *data=vertex?(const guchar*)m->vertices:
                preview?(const guchar*)m->preview_indices:(const guchar*)m->indices;
            /* Preview indices occupy the tail of the same IBO. Upload them
             * first, so a useful image appears while full indices stream in. */
            gsize offset=*sent+(preview?r->index_bytes:0);
            glBufferSubData(target,(GLintptr)offset,(GLsizeiptr)bytes,data+*sent);
            *sent+=bytes; available-=bytes; r->sent_bytes+=bytes;
            if (!gl_check(error,"Uploading model geometry")) return FALSE;
            continue;
        }
        if (r->texture_cursor>=r->texture_count) break;
        guint index=r->texture_cursor;
        Texture *t=&r->textures[index]; GdkPixbuf *image=m->materials[index].texture;
        if (!image || t->owner!=index) { r->texture_cursor++; continue; }
        glActiveTexture(GL_TEXTURE0); glBindSampler(0,0);
        if (!t->name) {
            glGenTextures(1,&t->name); glBindTexture(GL_TEXTURE_2D,t->name);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
            glTexImage2D(GL_TEXTURE_2D,0,t->channels==4?GL_SRGB8_ALPHA8:GL_SRGB8,t->width,t->height,0,
                t->channels==4?GL_RGBA:GL_RGB,GL_UNSIGNED_BYTE,NULL);
            if (!gl_check(error,"Allocating a model texture")) return FALSE;
        } else glBindTexture(GL_TEXTURE_2D,t->name);
        gsize row_bytes=(gsize)t->width*t->channels;
        guint rows=(guint)MIN((gsize)(t->height-t->rows),MAX((gsize)1,MIN(available,(gsize)2*1024*1024)/row_bytes));
        gsize bytes=row_bytes*rows;
        guchar *packed=g_malloc(bytes);
        const guchar *pixels=gdk_pixbuf_read_pixels(image);
        int stride=gdk_pixbuf_get_rowstride(image);
        for (guint row=0;row<rows;row++) memcpy(packed+row*row_bytes,pixels+(gsize)(t->rows+(int)row)*stride,row_bytes);
        glPixelStorei(GL_UNPACK_ALIGNMENT,1); glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
        glTexSubImage2D(GL_TEXTURE_2D,0,0,t->rows,t->width,(GLsizei)rows,
            t->channels==4?GL_RGBA:GL_RGB,GL_UNSIGNED_BYTE,packed);
        g_free(packed); t->rows+=(int)rows; r->sent_bytes+=bytes; available=bytes>=available?0:available-bytes;
        if (t->rows==t->height) { glGenerateMipmap(GL_TEXTURE_2D); t->ready=TRUE; r->texture_cursor++; }
        if (!gl_check(error,"Uploading a model texture")) return FALSE;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT,4); glBindVertexArray(0); glBindBuffer(GL_ARRAY_BUFFER,0);
    if (r->vertex_sent==r->vertex_bytes && r->preview_sent==r->preview_bytes &&
        r->index_sent==r->index_bytes && r->texture_cursor==r->texture_count) {
        r->pending=FALSE; r->cpu_released=TRUE;
        g_clear_pointer(&m->vertices,g_free); g_clear_pointer(&m->indices,g_free);
        g_clear_pointer(&m->preview_indices,g_free);
        for (guint i=0;i<m->material_count;i++) g_clear_object(&m->materials[i].texture);
    }
    return TRUE;
}

static float dot3(const float *a,const float *b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static void cross3(const float *a,const float *b,float *c) {
    c[0]=a[1]*b[2]-a[2]*b[1]; c[1]=a[2]*b[0]-a[0]*b[2]; c[2]=a[0]*b[1]-a[1]*b[0];
}
static gboolean normalize3(float *a) {
    float length=sqrtf(dot3(a,a)); if (!isfinite(length) || length<1e-7f) return FALSE;
    for (int i=0;i<3;i++) a[i]/=length;
    return TRUE;
}
static void camera_matrix(const Zen3dView *v,int width,int height,float *mvp) {
    float f[3],s[3],u[3],view[16]={0},projection[16]={0};
    for (int i=0;i<3;i++) f[i]=v->center[i]-v->eye[i];
    if (!normalize3(f)) { f[0]=0; f[1]=0; f[2]=-1; }
    cross3(f,v->up,s);
    if (!normalize3(s)) {
        const float alternate[3]={0,0,1}; cross3(f,alternate,s);
        if (!normalize3(s)) { s[0]=1; s[1]=s[2]=0; }
    }
    cross3(s,f,u);
    view[0]=s[0]; view[4]=s[1]; view[8]=s[2]; view[12]=-dot3(s,v->eye);
    view[1]=u[0]; view[5]=u[1]; view[9]=u[2]; view[13]=-dot3(u,v->eye);
    view[2]=-f[0]; view[6]=-f[1]; view[10]=-f[2]; view[14]=dot3(f,v->eye); view[15]=1;
    float delta[3]; for (int i=0;i<3;i++) delta[i]=v->eye[i]-v->center[i];
    float distance=sqrtf(dot3(delta,delta));
    float near_plane=MAX(.0005f,distance*.0005f),far_plane=MAX(50.f,distance+32.f);
    float aspect=(float)MAX(width,1)/MAX(height,1);
    if (v->orthographic) {
        float h=MAX(.0001f,v->ortho_height),w=h*aspect;
        projection[0]=2/w; projection[5]=2/h; projection[10]=-2/(far_plane-near_plane);
        projection[14]=-(far_plane+near_plane)/(far_plane-near_plane); projection[15]=1;
    } else {
        float angle=(v->vertical_fov>0?CLAMP(v->vertical_fov,5,120):38)*PI_F/180;
        float scale=1/tanf(angle*.5f);
        projection[0]=scale/aspect; projection[5]=scale;
        projection[10]=-(far_plane+near_plane)/(far_plane-near_plane); projection[11]=-1;
        projection[14]=-2*far_plane*near_plane/(far_plane-near_plane);
    }
    for (int col=0;col<4;col++) for (int row=0;row<4;row++) {
        float value=0; for (int i=0;i<4;i++) value+=projection[i*4+row]*view[col*4+i];
        mvp[col*4+row]=value;
    }
}
static void draw_batches(Zen3dRenderer *r,gboolean wire,gboolean preview) {
    const float fallback[4]={.66f,.76f,.80f,1};
    glUniform1i(r->mesh.wire,wire);
    for (guint i=0;i<(preview?1:r->batches->len);i++) {
        Batch *batch=g_ptr_array_index(r->batches,i);
        if (wire && batch->primitive!=GL_TRIANGLES) continue;
        guint material=batch->material;
        const float *color=material<r->model->material_count?r->model->materials[material].color:fallback;
        glUniform4fv(r->mesh.color,1,color);
        Texture *texture=material<r->texture_count?&r->textures[r->textures[material].owner]:NULL;
        gboolean textured=texture && texture->ready;
        glUniform1i(r->mesh.textured,textured);
        glBindTexture(GL_TEXTURE_2D,textured?texture->name:0);
        glUniform1i(r->mesh.primitive,batch->primitive==GL_TRIANGLES?0:batch->primitive==GL_LINES?1:2);
        if (preview)
            glDrawElements(GL_TRIANGLES,(GLsizei)r->model->preview_index_count,GL_UNSIGNED_INT,
                           (const void*)(uintptr_t)r->index_bytes);
        else
            glMultiDrawElements(batch->primitive,(const GLsizei*)batch->counts->data,GL_UNSIGNED_INT,
                               (const void *const*)batch->offsets->data,(GLsizei)batch->counts->len);
    }
}
gboolean zen3d_renderer_render(Zen3dRenderer *r,const Zen3dView *view,int width,int height,GError **error) {
    g_return_val_if_fail(r && view,FALSE);
    if (!r->initialized) return fail(error,"The 3D drawing context is not initialized.");
    drain_retired(r);
    if (width<1 || height<1) return TRUE;
    for (int i=0;i<3;i++) if (!isfinite(view->eye[i]) || !isfinite(view->center[i]) || !isfinite(view->up[i]))
        return fail(error,"The camera contains invalid coordinates.");
    if (r->dirty && !r->model) { clear_geometry(r); r->dirty=r->pending=FALSE; }
    if (!r->failed && !r->needs_reload && !upload_chunk(r,error)) { r->failed=TRUE; r->pending=FALSE; return FALSE; }
    gboolean full_ready=r->index_sent==r->index_bytes;
    gboolean preview_ready=r->preview_bytes && r->preview_sent==r->preview_bytes;
    gboolean drawable=!r->failed && r->vao && r->vertex_sent==r->vertex_bytes && (full_ready || preview_ready);
    if (drawable && r->previous) { destroy_previous_gl(r->previous); r->previous=NULL; }
    Zen3dRenderer *scene=drawable?r:r->previous;
    const Zen3dModel *bounds=scene?scene->model:r->model;
    float mvp[16]; camera_matrix(view,width,height,mvp);
    glViewport(0,0,width,height); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE); glDepthRange(0,1);
    glPolygonMode(GL_FRONT_AND_BACK,GL_FILL); glDisable(GL_RASTERIZER_DISCARD); glDisable(GL_PRIMITIVE_RESTART);
    glDisable(GL_POLYGON_OFFSET_FILL); glDisable(GL_POLYGON_OFFSET_LINE); glBindSampler(0,0);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST); glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glClearDepth(1);
    glClear(GL_DEPTH_BUFFER_BIT);
    glBindVertexArray(r->empty_vao); glUseProgram(r->background.name); glDrawArrays(GL_TRIANGLES,0,3);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    if (view->grid) {
        float floor=bounds?bounds->bounds_min[1]-.025f:-.8f;
        if (view->eye[1]>floor) {
            float extent[3]={.7f,.7f,.7f};
            if (bounds) for (int i=0;i<3;i++) extent[i]=MAX(.08f,(bounds->bounds_max[i]-bounds->bounds_min[i])*.5f);
            glUseProgram(r->ground.name); glUniformMatrix4fv(r->ground.mvp,1,GL_FALSE,mvp);
            glUniform1f(r->ground.floor,floor); glUniform3fv(r->ground.extent,1,extent); glUniform1f(r->ground.grid_scale,.2f);
            glDepthMask(GL_FALSE); glDrawArrays(GL_TRIANGLES,0,6); glDepthMask(GL_TRUE);
        }
    }
    if (scene) {
        gboolean use_preview=scene->preview_bytes && scene->preview_sent==scene->preview_bytes &&
            (view->interacting || scene->index_sent<scene->index_bytes);
        glBindVertexArray(scene->vao); glUseProgram(r->mesh.name); glEnable(GL_PROGRAM_POINT_SIZE);
        glUniformMatrix4fv(r->mesh.mvp,1,GL_FALSE,mvp); glUniform3fv(r->mesh.eye,1,view->eye);
        glActiveTexture(GL_TEXTURE0); draw_batches(scene,FALSE,use_preview);
        if (view->wireframe) {
            glPolygonMode(GL_FRONT_AND_BACK,GL_LINE); glEnable(GL_POLYGON_OFFSET_LINE); glPolygonOffset(-1,-1);
            draw_batches(scene,TRUE,use_preview); glDisable(GL_POLYGON_OFFSET_LINE); glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
        }
        glDisable(GL_PROGRAM_POINT_SIZE);
    }
    glDepthMask(GL_TRUE); glDisable(GL_BLEND); glBindVertexArray(0); glBindTexture(GL_TEXTURE_2D,0); glUseProgram(0);
    return gl_check(error,"Drawing the model");
}
