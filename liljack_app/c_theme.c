#define _XOPEN_SOURCE 700
#include "c_theme.h"
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <wchar.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
static const lj_theme_token defaults[LJ_THEME_COUNT]={
    [LJ_THEME_DIVIDER]={"divider",0x1c2b45u,0,0,""},
    [LJ_THEME_DIVIDER_HOT]={"divider-hot",0xffd54au,0,0,""},
    [LJ_THEME_WARNING]={"warning",0xe1efffu,0,0,"⚠"},
    [LJ_THEME_LOCK]={"lock",0xe1efffu,0,0,"🔒"},
    [LJ_THEME_CHAT]={"chat",0xe1efffu,0,0,"💬"},
    [LJ_THEME_UP_ARROW]={"up_arrow",0xe1efffu,0,0,"↑"},
    [LJ_THEME_DIRECTORY]={"directory",0xe1efffu,0,0,"▸"},
    [LJ_THEME_SYMLINK]={"symlink",0xe1efffu,0,0,"⇢"},
    [LJ_THEME_TREE_LAST]={"tree_last",0xe1efffu,0,0,"└"},
    [LJ_THEME_TREE_MID]={"tree_mid",0xe1efffu,0,0,"├"},

    [LJ_THEME_LAB_BG]={"lab_bg",0x08101au,0,0,""},
    [LJ_THEME_LAB_PANEL]={"lab_panel",0x101b29u,0,0,""},
    [LJ_THEME_LAB_EDGE]={"lab_edge",0x354354u,0,0,""},
    [LJ_THEME_LAB_SURF2]={"lab_surf2",0x203750u,0,0,""},
    [LJ_THEME_LAB_SURF3]={"lab_surf3",0x304760u,0,0,""},
    [LJ_THEME_LAB_TEXT]={"lab_text",0xe4eaf0u,0,0,""},
    [LJ_THEME_LAB_DIM]={"lab_dim",0x8798aau,0,0,""},
    [LJ_THEME_LAB_BLUE]={"lab_blue",0x4eb7ffu,0,0,""},
    [LJ_THEME_LAB_GOLD]={"lab_gold",0xffcf40u,0,0,""},
    [LJ_THEME_LAB_RED]={"lab_red",0xff6070u,0,0,""},
    [LJ_THEME_LAB_GREEN]={"lab_green",0x40df80u,0,0,""},
    [LJ_THEME_LAB_GRAPH_BG]={"lab_graph_bg",0x102030u,0,0,""},
    [LJ_THEME_LAB_HAIRLINE]={"lab_hairline",0x657586u,0,0,""},
    [LJ_THEME_LAB_WHITE]={"lab_white",0xffffffu,0,0,""},

    [LJ_THEME_RETURN_MARK]={"return_mark",0xe1efffu,0,0,"⏎"},
    [LJ_THEME_RULE_FILLED]={"rule_filled",0xe1efffu,0,0,"━"},
    [LJ_THEME_BG]={"bg",0x060b14u,0,0,""},
    [LJ_THEME_PANEL]={"panel",0x0d1626u,0,0,""},
    [LJ_THEME_EDGE]={"edge",0x1c2b45u,0,0,""},
    [LJ_THEME_SURF2]={"surf2",0x142238u,0,0,""},
    [LJ_THEME_SURF3]={"surf3",0x1e3a5eu,0,0,""},
    [LJ_THEME_GREEN]={"green",0xffd54au,0,0,""},
    [LJ_THEME_TEXT]={"text",0xe1efffu,0,0,""},
    [LJ_THEME_DIM]={"dim",0x8aaed0u,0,0,""},
    [LJ_THEME_CYAN]={"cyan",0x5bbcffu,0,0,""},
    [LJ_THEME_AMBER]={"amber",0xffd54au,0,0,""},
    [LJ_THEME_RED]={"red",0xff7373u,0,0,""},
    [LJ_THEME_BUTTON_ACTIVE]={"button_active",0x19466du,0,0,""},
    [LJ_THEME_BUTTON_IDLE]={"button_idle",0x102d4bu,0,0,""},
    [LJ_THEME_ENDED]={"ended",0x4a6280u,0,0,""},
    [LJ_THEME_UNAVAILABLE_BG]={"unavailable_bg",0x2a1420u,0,0,""},
    [LJ_THEME_DISABLED]={"disabled",0x44607fu,0,0,""},
    [LJ_THEME_TASK_ACTIVE]={"task_active",0x8fd98fu,0,0,""},
    [LJ_THEME_TASK_META]={"task_meta",0x6b8299u,0,0,""},
    [LJ_THEME_DEPENDENCY]={"dependency",0x8a7a4au,0,0,""},
    [LJ_THEME_CANVAS_FG]={"canvas_fg",0xb0ffb0u,0,0,""},
    [LJ_THEME_CANVAS_BG]={"canvas_bg",0x030b03u,0,0,""},
    [LJ_THEME_NOTICE_BG]={"notice_bg",0x2a1d00u,0,0,""},
    [LJ_THEME_NOTICE_FG]={"notice_fg",0xffc94au,0,0,""},
    [LJ_THEME_SCROLLBAR]={"scrollbar",0x7fa5c4u,0,0,""},
    [LJ_THEME_GRAPH_LOW]={"graph_low",0x88eba5u,0,0,""},
    [LJ_THEME_GRAPH_MID]={"graph_mid",0xe7c67eu,0,0,""},
    [LJ_THEME_GRAPH_HIGH]={"graph_high",0xe47676u,0,0,""},
    [LJ_THEME_ANSI_0]={"ansi_0",0x08130fu,0,0,""},
    [LJ_THEME_ANSI_1]={"ansi_1",0xe47676u,0,0,""},
    [LJ_THEME_ANSI_2]={"ansi_2",0x88eba5u,0,0,""},
    [LJ_THEME_ANSI_3]={"ansi_3",0xe7c67eu,0,0,""},
    [LJ_THEME_ANSI_4]={"ansi_4",0x85adf5u,0,0,""},
    [LJ_THEME_ANSI_5]={"ansi_5",0xc6a0e8u,0,0,""},
    [LJ_THEME_ANSI_6]={"ansi_6",0x8cd8d3u,0,0,""},
    [LJ_THEME_ANSI_7]={"ansi_7",0xd5e9dcu,0,0,""},
    [LJ_THEME_ANSI_8]={"ansi_8",0x6e8e79u,0,0,""},
    [LJ_THEME_ANSI_9]={"ansi_9",0xff9999u,0,0,""},
    [LJ_THEME_ANSI_10]={"ansi_10",0xacffc2u,0,0,""},
    [LJ_THEME_ANSI_11]={"ansi_11",0xffdf9au,0,0,""},
    [LJ_THEME_ANSI_12]={"ansi_12",0xa6c9ffu,0,0,""},
    [LJ_THEME_ANSI_13]={"ansi_13",0xe0b8ffu,0,0,""},
    [LJ_THEME_ANSI_14]={"ansi_14",0xaff9edu,0,0,""},
    [LJ_THEME_ANSI_15]={"ansi_15",0xf4fff8u,0,0,""},
    [LJ_THEME_ICON_CLAUDE]={"icon_claude",0xe1efffu,0,0,"🧠"},
    [LJ_THEME_ICON_CODEX]={"icon_codex",0xe1efffu,0,0,"🛠"},
    [LJ_THEME_ICON_DEEPSEEK]={"icon_deepseek",0xe1efffu,0,0,"🔎"},
    [LJ_THEME_ICON_SHELL]={"icon_shell",0xe1efffu,0,0,"🌿"},
    [LJ_THEME_ICON_USER]={"icon_user",0xe1efffu,0,0,"👤"},
    [LJ_THEME_ROOM]={"room",0xe1efffu,0,0,"▣"},
    [LJ_THEME_STANDALONE]={"standalone",0xe1efffu,0,0,"◇"},
    [LJ_THEME_CHECK]={"check",0xe1efffu,0,0,"✓"},
    [LJ_THEME_ACTIVE]={"active",0xe1efffu,0,0,"●"},
    [LJ_THEME_BLOCKED]={"blocked",0xe1efffu,0,0,"■"},
    [LJ_THEME_EMPTY]={"empty",0xe1efffu,0,0,"○"},
    [LJ_THEME_CLOSE]={"close",0xe1efffu,0,0,"×"},
    [LJ_THEME_RESIZE]={"resize",0xe1efffu,0,0,"↘"},
    [LJ_THEME_DISCONNECTED]={"disconnected",0xe1efffu,0,0,"◉"},
    [LJ_THEME_UP]={"up",0xe1efffu,0,0,"▴"},
    [LJ_THEME_DOWN]={"down",0xe1efffu,0,0,"▾"},
    [LJ_THEME_ELLIPSIS]={"ellipsis",0xe1efffu,0,0,"…"},
    [LJ_THEME_RING_DOT]={"ring_dot",0xe1efffu,0,0,"•"},
    [LJ_THEME_RULE_H]={"rule_h",0xe1efffu,0,0,"─"},
    [LJ_THEME_RULE_V]={"rule_v",0xe1efffu,0,0,"│"},
    [LJ_THEME_SCROLL_THUMB]={"scroll_thumb",0xe1efffu,0,0,"▐"},
    [LJ_THEME_SCROLL_TRACK]={"scroll_track",0xe1efffu,0,0,"▕"},
    [LJ_THEME_PROMPT]={"prompt",0xe1efffu,0,0,"▶"},
    [LJ_THEME_PROMPT_CURSOR]={"prompt_cursor",0xe1efffu,0,0,"▎"},
};
static lj_theme_token current[LJ_THEME_COUNT];
static int initialized;
static uint32_t ansi_color(unsigned n){
    static const uint32_t basic[]={0x000000,0x800000,0x008000,0x808000,0x000080,0x800080,0x008080,0xc0c0c0,
        0x808080,0xff0000,0x00ff00,0xffff00,0x0000ff,0xff00ff,0x00ffff,0xffffff};
    if(n<16)return basic[n];
    if(n>=232){unsigned v=8+10*(n-232);return v*0x010101;}
    n-=16;unsigned r=n/36,g=n/6%6,b=n%6;
    return ((r?55+40*r:0)<<16)|((g?55+40*g:0)<<8)|(b?55+40*b:0);
}
static unsigned nearest(uint32_t rgb,unsigned count){
    unsigned best=0,distance=UINT_MAX;
    for(unsigned i=0;i<count;i++){
        uint32_t c=ansi_color(i);int r=(int)(rgb>>16)-(int)(c>>16);
        int g=(int)((rgb>>8)&255)-(int)((c>>8)&255),b=(int)(rgb&255)-(int)(c&255);
        unsigned d=(unsigned)(r*r+g*g+b*b);if(d<distance){best=i;distance=d;}
    }return best;
}
static void map(lj_theme_token *t){t->ansi16=nearest(t->rgb,16);t->ansi256=nearest(t->rgb,256);}
static const lj_theme_setting setting_defaults[LJ_THEME_SETTING_COUNT]={
    {"graph.bucket_ms","ms",50,25,1000,25,50},
    {"graph.repaint_ms","ms",100,50,2000,25,100},
    {"metrics.poll_ms","ms",25,25,1000,25,25},
    {"metrics.label_hold_ms","ms",500,100,5000,100,500},
    {"divider.idle_px","px",1,1,3,1,1},
    {"divider.hot_px","px",3,1,6,1,3},
    {"tab.badge_min_cells","cells",6,4,12,1,6},
    {"effects.scanlines","bool",0,0,1,1,0}
};
static lj_theme_setting settings[LJ_THEME_SETTING_COUNT];
const lj_theme_setting *lj_theme_setting_get(lj_theme_setting_id id){
    if(!initialized)lj_theme_reset();return id>=0&&id<LJ_THEME_SETTING_COUNT?&settings[id]:NULL;
}
int lj_theme_int(lj_theme_setting_id id){const lj_theme_setting*t=lj_theme_setting_get(id);return t?t->value:-1;}
int lj_theme_set_int(lj_theme_setting_id id,int value){
    if(!lj_theme_setting_get(id))return -1;
    if(value<settings[id].min)value=settings[id].min;
    if(value>settings[id].max)value=settings[id].max;
    settings[id].value=value;
    if(settings[LJ_THEME_GRAPH_REPAINT_MS].value<settings[LJ_THEME_GRAPH_BUCKET_MS].value)
        settings[LJ_THEME_GRAPH_REPAINT_MS].value=settings[LJ_THEME_GRAPH_BUCKET_MS].value;
    return settings[id].value;
}
static int parse_settings(json_object*root,lj_theme_setting*out){
    memcpy(out,setting_defaults,sizeof setting_defaults);json_object*all=NULL;
    if(!json_object_object_get_ex(root,"settings",&all))return 1;
    if(!json_object_is_type(all,json_type_object))return 0;
    json_object_object_foreach(all,name,obj){
        int id=-1;for(int i=0;i<LJ_THEME_SETTING_COUNT;i++)if(!strcmp(name,out[i].name))id=i;
        if(id<0||!json_object_is_type(obj,json_type_object))return 0;
        json_object*v=NULL;if(!json_object_object_get_ex(obj,"int",&v)||!json_object_is_type(v,json_type_int))return 0;
        int64_t n=json_object_get_int64(v);if(n<out[id].min||n>out[id].max)return 0;
        out[id].value=(int)n;
        json_object_object_foreach(obj,key,val){
            if(!strcmp(key,"int"))continue;
            if(!strcmp(key,"unit")){
                if(!json_object_is_type(val,json_type_string)||(size_t)json_object_get_string_len(val)!=strlen(out[id].unit)||strcmp(json_object_get_string(val),out[id].unit))return 0;
            }else if(!strcmp(key,"min")||!strcmp(key,"max")){
                int expected=!strcmp(key,"min")?out[id].min:out[id].max;
                if(!json_object_is_type(val,json_type_int)||json_object_get_int64(val)!=expected)return 0;
            }else return 0;
        }
    }
    if(out[1].value<out[0].value)out[1].value=out[0].value;
    return 1;
}
static void serialize_settings(json_object*root,const lj_theme_setting*values){
    json_object*all=json_object_new_object();json_object_object_add(root,"settings",all);
    for(int i=0;i<LJ_THEME_SETTING_COUNT;i++){
        json_object*t=json_object_new_object();json_object_object_add(all,values[i].name,t);
        json_object_object_add(t,"int",json_object_new_int(values[i].value));
        json_object_object_add(t,"min",json_object_new_int(values[i].min));
        json_object_object_add(t,"max",json_object_new_int(values[i].max));
        json_object_object_add(t,"unit",json_object_new_string(values[i].unit));
    }
}
void lj_theme_reset(void){memcpy(settings,setting_defaults,sizeof settings);memcpy(current,defaults,sizeof current);for(int i=0;i<LJ_THEME_COUNT;i++)map(&current[i]);initialized=1;}
const lj_theme_token *lj_theme_get(lj_theme_id id){if(!initialized)lj_theme_reset();return id>=0&&id<LJ_THEME_COUNT?&current[id]:NULL;}
uint32_t lj_theme_rgb(lj_theme_id id){const lj_theme_token*t=lj_theme_get(id);return t?t->rgb:0;}
uint32_t lj_theme_set_rgb(lj_theme_id id,uint32_t rgb){
    if(!lj_theme_get(id))return 0;
    current[id].rgb=rgb&0xffffffu;map(&current[id]);return current[id].rgb;
}
const char *lj_theme_glyph(lj_theme_id id){const lj_theme_token*t=lj_theme_get(id);return t?t->glyph:"";}
uint32_t lj_theme_codepoint(lj_theme_id id){
    const unsigned char*s=(const unsigned char*)lj_theme_glyph(id);uint32_t cp=*s;int n=1;
    if(cp>=240){cp&=7;n=4;}else if(cp>=224){cp&=15;n=3;}else if(cp>=192){cp&=31;n=2;}
    for(int i=1;i<n;i++)cp=(cp<<6)|(s[i]&63);return cp;
}
int lj_theme_find(const char *name){if(!name)return -1;for(int i=0;i<LJ_THEME_COUNT;i++)if(!strcmp(defaults[i].name,name))return i;return -1;}
static int fail(char *error,size_t cap,const char *message){if(error&&cap)snprintf(error,cap,"%s",message);return -1;}
static int rgb_parse(const char *s,uint32_t *out){
    if(strlen(s)!=7||*s!='#')return 0;uint32_t n=0;
    for(int i=1;i<7;i++){unsigned char c=(unsigned char)s[i];int v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;if(v<0)return 0;n=n*16+(unsigned)v;}
    *out=n;return 1;
}
/* Printable, valid UTF-8 only. JSON strings must not inject terminal controls.
 * Fixed-width decorative substitutions retain the default cell width. */
static int glyph_width(const char *s){
    mbstate_t st={0};int width=0;while(*s){wchar_t cp;size_t n=mbrtowc(&cp,s,strlen(s),&st);
        if(n==(size_t)-1||n==(size_t)-2||!n)return -1;
        int w=wcwidth(cp);if(w<0)return -1;width+=w;s+=n;
    }return width;
}
int lj_theme_load(const char *path,char *error,size_t cap){
    if(error&&cap)*error=0;if(!path||!*path)return fail(error,cap,"theme path is empty");
    FILE*f=fopen(path,"rb");if(!f)return errno==ENOENT?0:fail(error,cap,"cannot open theme file");
    char *data=malloc(65538);if(!data){fclose(f);return fail(error,cap,"theme allocation failed");}
    size_t n=fread(data,1,65537,f);int bad=ferror(f);fclose(f);
    if(bad||n>65536){free(data);return fail(error,cap,"theme read failed or exceeds 64 KiB");}data[n]=0;
    json_tokener *tok=json_tokener_new();if(!tok){free(data);return fail(error,cap,"theme parser allocation failed");}
    json_tokener_set_flags(tok,JSON_TOKENER_STRICT);
    json_object *root=json_tokener_parse_ex(tok,data,(int)n);size_t end=json_tokener_get_parse_end(tok);
    int valid=json_tokener_get_error(tok)==json_tokener_success;while(end<n&&isspace((unsigned char)data[end]))end++;
    json_tokener_free(tok);free(data);
    json_object *tokens=NULL,*version=NULL;
    if(!valid||end!=n||!root||!json_object_is_type(root,json_type_object)||
       !json_object_object_get_ex(root,"version",&version)||!json_object_is_type(version,json_type_int)||json_object_get_int(version)!=1||
       !json_object_object_get_ex(root,"tokens",&tokens)||!json_object_is_type(tokens,json_type_object)){
        if(root)json_object_put(root);return fail(error,cap,"expected version 1 theme object with tokens");}
    lj_theme_token candidate[LJ_THEME_COUNT];memcpy(candidate,defaults,sizeof candidate);
    json_object_object_foreach(tokens,name,object){
        int id=lj_theme_find(name);if(id<0||!json_object_is_type(object,json_type_object)){valid=0;break;}
        json_object_object_foreach(object,key,value){
            if(!json_object_is_type(value,json_type_string)){valid=0;break;}
            const char*s=json_object_get_string(value);size_t len=(size_t)json_object_get_string_len(value);
            if(strlen(s)!=len){valid=0;break;}
            if(!strcmp(key,"rgb")){if(!rgb_parse(s,&candidate[id].rgb)){valid=0;break;}}
            else if(!strcmp(key,"glyph")){
                int width=glyph_width(s),expected=glyph_width(defaults[id].glyph);
                mbstate_t state={0};wchar_t scalar;size_t scalar_bytes=len?mbrtowc(&scalar,s,len,&state):0;
                if(len>=sizeof candidate[id].glyph||width<0||width!=expected||scalar_bytes!=len){valid=0;break;}
                memcpy(candidate[id].glyph,s,len+1);
            }else {valid=0;break;}
        }if(!valid)break;
    }
    lj_theme_setting next_settings[LJ_THEME_SETTING_COUNT];
    if(valid)valid=parse_settings(root,next_settings);
    json_object_put(root);if(!valid)return fail(error,cap,"invalid token, glyph, or numeric setting");
    memcpy(settings,next_settings,sizeof settings);
    for(int i=0;i<LJ_THEME_COUNT;i++)map(&candidate[i]);memcpy(current,candidate,sizeof current);initialized=1;return 1;
}
int lj_theme_load_default(char *error,size_t cap){
    const char *explicit_path=getenv("LILJACK_THEME");if(explicit_path&&*explicit_path)return lj_theme_load(explicit_path,error,cap);
    const char *base=getenv("XDG_CONFIG_HOME"),*home=getenv("HOME");char path[PATH_MAX];int n;
    if(base&&*base)n=snprintf(path,sizeof path,"%s/liljack/theme.json",base);
    else if(home&&*home)n=snprintf(path,sizeof path,"%s/.config/liljack/theme.json",home);
    else return 0;
    if(n<0||(size_t)n>=sizeof path)return fail(error,cap,"theme path too long");return lj_theme_load(path,error,cap);
}
static int write_theme(const char *path,int use_defaults){
    if(!initialized)lj_theme_reset();
    const lj_theme_token*values=use_defaults?defaults:current;
    json_object *root=json_object_new_object(),*tokens=json_object_new_object();
    if(!root||!tokens){if(root)json_object_put(root);if(tokens)json_object_put(tokens);return -1;}
    json_object_object_add(root,"version",json_object_new_int(1));json_object_object_add(root,"tokens",tokens);
    for(int i=0;i<LJ_THEME_COUNT;i++){
        json_object *t=json_object_new_object();char rgb[8];snprintf(rgb,sizeof rgb,"#%06x",values[i].rgb);
        json_object_object_add(t,"rgb",json_object_new_string(rgb));json_object_object_add(t,"glyph",json_object_new_string(values[i].glyph));
        json_object_object_add(tokens,defaults[i].name,t);
    }
    serialize_settings(root,use_defaults?setting_defaults:settings);
    int result=json_object_to_file_ext(path,root,JSON_C_TO_STRING_PRETTY);json_object_put(root);return result;
}

int lj_theme_write_defaults(const char*path){return write_theme(path,1);}
int lj_theme_save_user(char*error,size_t cap){
    if(error&&cap)*error=0;
    const char*explicit_path=getenv("LILJACK_THEME"),*base=getenv("XDG_CONFIG_HOME"),*home=getenv("HOME");
    char path[PATH_MAX],temp[PATH_MAX];int n;
    if(explicit_path&&*explicit_path)n=snprintf(path,sizeof path,"%s",explicit_path);
    else if(base&&*base)n=snprintf(path,sizeof path,"%s/liljack/theme.json",base);
    else if(home&&*home)n=snprintf(path,sizeof path,"%s/.config/liljack/theme.json",home);
    else return fail(error,cap,"no user theme path");
    if(n<0||(size_t)n>=sizeof path)return fail(error,cap,"theme path too long");
    for(char*p=path+1;*p;p++)if(*p=='/'){
        *p=0;int rc=mkdir(path,0700),saved=errno;*p='/';
        if(rc&&saved!=EEXIST)return fail(error,cap,"cannot create theme directory");
    }
    n=snprintf(temp,sizeof temp,"%s.XXXXXX",path);
    if(n<0||(size_t)n>=sizeof temp)return fail(error,cap,"theme path too long");
    int fd=mkstemp(temp);if(fd<0)return fail(error,cap,"cannot create theme temporary file");
    close(fd);
    if(write_theme(temp,0)<0||rename(temp,path)<0){unlink(temp);return fail(error,cap,"cannot save theme");}
    return 1;
}
