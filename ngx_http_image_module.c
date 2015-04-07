/*
* Copyright (C) Vampire
*
* {根据URL生成缩略图/添加水印}
*
* nginx.conf 配置值
* image on/off 是否开启缩略图功能,默认关闭
* image_backend on/off 是否开启镜像服务
* image_backend_server 镜像服务器地址
* image_output on/off 是否不生成图片而直接处理后输出 默认off
* image_jpeg_quality 75 生成JPEG图片的质量 默认值75
* image_water on/off 是否开启水印功能
* image_water_type 0/1 水印类型 0:图片水印 1:文字水印
* image_water_min 300 300 图片宽度 300 高度 300 的情况才添加水印
* image_water_pos 0-9 水印位置 默认值9 0为随机位置,1为顶端居左,2为顶端居中,3为顶端居右,4为中部居左,5为中部居中,6为中部居右,7为底端居左,8为底端居中,9为底端居右
* image_water_file 水印文件(jpg/png/gif),绝对路径或者相对路径的水印图片
* image_water_transparent 水印透明度,默认20
* image_water_text 水印文字 "Power By Vampire"
* image_water_font_size 水印大小 默认 5
* image_water_font;//文字水印字体文件路径
* image_water_color 水印文字颜色,默认 #000000
*/

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_files.h>
#include "ngx_automem.h"
#include <string.h>
#include <gd.h>
#include <gdfontg.h>
#include <time.h>

#if defined(_WIN32) || defined(_WIN64) || defined(WIN32)  || defined(WIN64)
	#include <direct.h>
	#include <io.h>
	#define stricmp	_stricmp
	#define strdup _strdup
	#define access(a, b) _access(a, b)
	#define mkdir(x, y)		_mkdir( x )
#else
	#include <dirent.h>
	#include <sys/types.h>
#endif

#include <pcre.h>
#include <curl/curl.h>
#include "ngx_http_request.h"

#define NGX_IMAGE_NONE      0
#define NGX_IMAGE_JPEG      1
#define NGX_IMAGE_GIF       2
#define NGX_IMAGE_PNG       3
#define NGX_IMAGE_BMP       4

#ifndef WIN32
#define stricmp strcasecmp
#endif

static u_char URL_PARTTEN[] = "([^<]*)\\/([^<]*)!([a-z])(\\d{2,4})x(\\d{2,4}).([a-zA-Z]{3,4})";
static char * get_file_contents(const char* file_name, size_t * len);

typedef struct
{
    ngx_flag_t image_status;//是否打开图片处理


	char * extension;//目标图片后缀名 (png/gif/jpg/jpeg/jpe)
	char * m_type;//生成缩略图的方式 缩放/居中缩放/顶部10%开始缩放


	//图片正则匹配类型 0为新规则(test.jpg!c300x300.jpg) 1为旧规则(test.c300x300.jpg)
	int header_type;//HTTP头部类型
	ngx_flag_t image_output;//是否不保存图片直接输出图片内容给客户端 默认off
	int jpeg_quality;//JPEG图片质量 默认75
	ngx_flag_t water_status;//是否打开水印功能 默认关闭

	int water_type;//水印类型 0:图片水印 1:文字水印
	int water_pos;//水印位置
	int water_transparent;//水印透明度
	int water_width_min;//原图小于该宽度的图片不添加水印
	int water_height_min;//原图小于该高度的图片不添加水印
	ngx_flag_t backend;//是否请求原始服务器
	ngx_str_t backend_server;//图片原始服务器,如请求的图片不存在,从该服务器下载
	ngx_str_t water_image;//水印图片
	ngx_str_t water_text;//水印文字内容
	int water_font_size;//水印文字大小
	ngx_str_t water_font;//文字水印字体文件路径
	ngx_str_t water_color;//水印文字颜色 (#0000000)
#if defined(DEBUG_CONSILE_TEST)
	pcre * regx_expr;
#else
	ngx_regex_t * regx_expr;
#endif
} ngx_image_conf_t;

typedef struct {
	char * url;//请求URL地址
	char * request_dir;//URL目录
	ngx_str_t dest_file;
	char * request_source;//URL源文件URL
	char * request_filename;//URL中的文件名

	char * source_file;//原始图片路径

	char * local_dir;//当前WEB目录

	gdImagePtr src_im;//原始图片GD对象
	gdImagePtr w_im;//补白边图片GD对象
	gdImagePtr dst_im;//目标图片GD对象
	gdImagePtr water_im;//水印图片GD对象

	int img_size;//图片大小

	int w_margin;//是否对图片补白边
	int max_width;//目标图片最大宽度
	int max_height;//目标图片最大宽度
	int src_type;//原始图片类型
	int dest_type;//目标图片类型
	int src_width;//原始图片宽度
	int src_height;//原始图片高度
	int src_x;//原始图片X坐标
	int src_y;//原始图片Y坐标
	int src_w;//原始图片宽度
	int src_h;//原始图片高度
	int width;//目标图片宽度
	int height;//目标图片高度
	int dst_x;//目标图片X坐标
	int dst_y;//目标图片Y坐标
	u_char * img_data;//图片内容

	int water_im_type;//水印图片类型

	ngx_image_conf_t * conf;
	ngx_http_request_t *r;
}ngx_image_thumb_context_t;

static FILE *curl_handle;

#if defined(DEBUG_CONSILE_TEST)
	const char REQUEST_URI_CONST[] = "test/7.jpg!w800x600.png";
	#define  ngx_alloc(a, b) malloc(a)
	#define ngx_free( x )	free( x )
#endif
static ngx_str_t  ngx_http_image_types[] =
{
        ngx_string("text/html"),
		ngx_string("image/jpeg"),
		ngx_string("image/gif"),
		ngx_string("image/png")
};

static char *ngx_http_image(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void *ngx_http_image_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_image_merge_loc_conf(ngx_conf_t *cf,void *parent, void *child);
static char * ngx_http_image_water_min(ngx_conf_t *cf, ngx_command_t *cmd,void *conf);
static ngx_uint_t ngx_http_image_value(ngx_str_t *value);
char * ngx_conf_set_number_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
char * ngx_conf_set_string_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t output(ngx_http_request_t *r,void *conf,ngx_str_t type);
static void gd_clean_data(void *data);//清除GD DATA数据
static void make_thumb(ngx_image_thumb_context_t *ctx);//创建GD对象缩略图,缩略图在此函数中已经处理好,但没有写入到文件
static void water_mark(ngx_image_thumb_context_t * ctx);//给图片打上水印
static void thumb_to_string(ngx_image_thumb_context_t * ctx);//GD对象数据转换为二进制字符串
static int parse_image_info(ngx_image_thumb_context_t *ctx);//根据正则获取基本的图片信息
static int calc_image_info(ngx_image_thumb_context_t *ctx);//根据基本图片信息计算缩略图信息
static void check_image_type(ngx_image_thumb_context_t *conf);//判断图片类型
static char * get_ext(char *filename);//根据文件名取图片类型
static int get_ext_header(char *filename);//根据文件头取图片类型
static int file_exists(char *filename);//判断文件是否存在
static void write_img(ngx_image_thumb_context_t * ctx);//图片保存到文件
static gdImagePtr read_image(const char * filename, int * ftype);
static void water_image_from(ngx_image_thumb_context_t * ctx);//创建水印图片GD对象
static void image_from(ngx_image_thumb_context_t * ctx);//创建原图GD库对象
static int get_header(char *url);//取得远程URL的返回值
static size_t curl_get_data(void *ptr, size_t size, size_t nmemb, void *stream);//CURL调用函数
static int create_dir(char *dir);//递归创建目录
static void get_request_source(void *conf);
static int dirname(char *path,char **dirpath);//根据URL获取目录路径
static void download(ngx_image_thumb_context_t * ctx);//下载文件

#if defined(DEBUG_CONSILE_TEST)
ngx_int_t ngx_http_discard_request_body(ngx_http_request_t *r){
	return NGX_OK;
}
u_char *ngx_http_map_uri_to_path(ngx_http_request_t *r, ngx_str_t *name, size_t *root_length, size_t reserved)
{
	name->len = sizeof("d:/site/pay/test/7.jpg!w800x600.jpg");
	name->data = (u_char *)"d:/site/pay/test/7.jpg!w800x600.jpg";
	*root_length = sizeof("d:/site/pay/")-1;
	return (u_char *)1;
}
static ngx_int_t output(ngx_http_request_t *r,void *conf,ngx_str_t type){
	return NGX_OK;
}

static void log_error(int a, ngx_log_t * log, int b, const char * fmt,...){
	va_list ap;
//	va_start(ap, fmt);
//	vprintf(fmt, ap);
//	va_end(ap);
	puts("");
}

#else
	#define log_error ngx_log_error

#endif
#if !defined(DEBUG_CONSILE_TEST)
static ngx_command_t  ngx_http_image_commands[] =
{
	{
		ngx_string("image"),
			NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
			ngx_http_image,
			NGX_HTTP_LOC_CONF_OFFSET,
			0,
			NULL
	},
	{
		ngx_string("image_output"),
			NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
			ngx_conf_set_flag_slot,
			NGX_HTTP_LOC_CONF_OFFSET,
			offsetof(ngx_image_conf_t, image_output),
			NULL
	},
		{
			ngx_string("image_backend"),
				NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
				ngx_conf_set_flag_slot,
				NGX_HTTP_LOC_CONF_OFFSET,
				offsetof(ngx_image_conf_t, backend),
				NULL
		},
		{
			ngx_string("image_backend_server"),
				NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
				ngx_conf_set_str_slot,
				NGX_HTTP_LOC_CONF_OFFSET,
				offsetof(ngx_image_conf_t, backend_server),
				NULL
		},
			{
				ngx_string("image_water"),
					NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
					ngx_conf_set_flag_slot,
					NGX_HTTP_LOC_CONF_OFFSET,
					offsetof(ngx_image_conf_t, water_status),
					NULL
			},
			{
				ngx_string("image_water_type"),
					NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_ARGS_NUMBER,
					ngx_conf_set_number_slot,
					NGX_HTTP_LOC_CONF_OFFSET,
					offsetof(ngx_image_conf_t, water_type),
					NULL
			},
				{
					ngx_string("image_water_min"),
						NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
						ngx_http_image_water_min,
						NGX_HTTP_LOC_CONF_OFFSET,
						0,
						NULL
				},
				{
					ngx_string("image_water_pos"),
						NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_ARGS_NUMBER,
						ngx_conf_set_number_slot,
						NGX_HTTP_LOC_CONF_OFFSET,
						offsetof(ngx_image_conf_t, water_pos),
						NULL
				},
					{
						ngx_string("image_water_file"),
							NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
							ngx_conf_set_str_slot,
							NGX_HTTP_LOC_CONF_OFFSET,
							offsetof(ngx_image_conf_t, water_image),
							NULL
					},
					{
						ngx_string("image_water_transparent"),
							NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
							ngx_conf_set_number_slot,
							NGX_HTTP_LOC_CONF_OFFSET,
							offsetof(ngx_image_conf_t, water_transparent),
							NULL
					},
						{
							ngx_string("image_water_text"),
								NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
								ngx_conf_set_str_slot,
								NGX_HTTP_LOC_CONF_OFFSET,
								offsetof(ngx_image_conf_t, water_text),
								NULL
						},
						{
							ngx_string("image_water_font"),
								NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
								ngx_conf_set_str_slot,
								NGX_HTTP_LOC_CONF_OFFSET,
								offsetof(ngx_image_conf_t, water_font),
								NULL
						},
							{
								ngx_string("image_water_font_size"),
									NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_ARGS_NUMBER,
									ngx_conf_set_number_slot,
									NGX_HTTP_LOC_CONF_OFFSET,
									offsetof(ngx_image_conf_t, water_font_size),
									NULL
							},
							{
								ngx_string("image_water_color"),
									NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
									ngx_conf_set_str_slot,
									NGX_HTTP_LOC_CONF_OFFSET,
									offsetof(ngx_image_conf_t, water_color),
									NULL
							},
								{
									ngx_string("image_jpeg_quality"),
										NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_ARGS_NUMBER,
										ngx_conf_set_number_slot,
										NGX_HTTP_LOC_CONF_OFFSET,
										offsetof(ngx_image_conf_t, jpeg_quality),
										NULL
								},
								ngx_null_command
};



static ngx_http_module_t  ngx_http_image_module_ctx =
{
	NULL,								// preconfiguration
		NULL,							// postconfiguration
										
		NULL,							// create main configuration
		NULL,							// init main configuration
										//
		NULL,							// create server configuration
		NULL,							// merge server configuration

		ngx_http_image_create_loc_conf,                         // create location configuration
		ngx_http_image_merge_loc_conf                           // merge location configuration 
};


ngx_module_t  ngx_http_image_module =
{
	NGX_MODULE_V1,
		&ngx_http_image_module_ctx,    // module context
		ngx_http_image_commands,       // module directives 
		NGX_HTTP_MODULE,               // module type 
		NULL,                          // init master 
		NULL,                          // init module 
		NULL,                          // init process
		NULL,                          // init thread 
		NULL,                          // exit thread 
		NULL,                          // exit process
		NULL,                          // exit master 
		NGX_MODULE_V1_PADDING
};
#endif


static void * ngx_http_image_create_loc_conf(ngx_conf_t *cf)
{

	ngx_image_conf_t  * conf = (ngx_image_conf_t  *)ngx_pcalloc(cf->pool, sizeof(ngx_image_conf_t));

	if (conf != NULL)
	{
		conf->image_status = NGX_CONF_UNSET;
		conf->backend = NGX_CONF_UNSET;
		conf->jpeg_quality = NGX_CONF_UNSET;
		conf->image_output = NGX_CONF_UNSET;
		conf->water_status = NGX_CONF_UNSET;
		conf->water_type = NGX_CONF_UNSET;
		conf->water_pos = NGX_CONF_UNSET;
		conf->water_transparent = NGX_CONF_UNSET;
		conf->water_width_min = NGX_CONF_UNSET;
		conf->water_height_min = NGX_CONF_UNSET;
		conf->water_font_size = NGX_CONF_UNSET;
	}
	return conf;
}

static char * ngx_http_image_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
	ngx_regex_compile_t   rc;
	u_char                errstr[NGX_MAX_CONF_ERRSTR];

	ngx_image_conf_t  * prev = (ngx_image_conf_t  *)parent;
	ngx_image_conf_t  *conf = (ngx_image_conf_t  *)child;

	ngx_conf_merge_value(conf->image_status,prev->image_status,0);
	ngx_conf_merge_value(conf->backend,prev->backend,0);
	ngx_conf_merge_str_value(conf->backend_server,prev->backend_server, NULL);
	ngx_conf_merge_value(conf->jpeg_quality,prev->jpeg_quality,75);
	ngx_conf_merge_value(conf->water_status,prev->water_status,0);
	ngx_conf_merge_value(conf->water_type,prev->water_type,0);
	ngx_conf_merge_value(conf->water_width_min,prev->water_width_min,0);
	ngx_conf_merge_value(conf->water_height_min,prev->water_height_min,0);
	ngx_conf_merge_value(conf->water_pos,prev->water_pos,9);
	ngx_conf_merge_value(conf->water_transparent,prev->water_transparent,20);
	ngx_conf_merge_str_value(conf->water_text,prev->water_text,"[ Copyright By Vampire ]");
	ngx_conf_merge_value(conf->water_font_size,prev->water_font_size,5);
	ngx_conf_merge_str_value(conf->water_font,prev->water_font,"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc");
	ngx_conf_merge_str_value(conf->water_color,prev->water_color,"#000000");

	rc.pool = cf->pool;
	rc.pattern.data =URL_PARTTEN;
	rc.pattern.len = sizeof(URL_PARTTEN);
	rc.err.len = NGX_MAX_CONF_ERRSTR;
	rc.err.data = errstr;
	rc.options=0;
	if (ngx_regex_compile(&rc) != NGX_OK)
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "%V", &rc.err);
		return NGX_CONF_ERROR;
	}
	conf->regx_expr = rc.regex;

	return NGX_CONF_OK;
}


static ngx_int_t ngx_http_image_handler(ngx_http_request_t *r)
{
	u_char                    *last;
	size_t                     root;
	ngx_int_t                  rc;
	u_char					*  request_uri;
	int						   request_uri_len;
	ngx_image_conf_t  *conf;

#if !defined(DEBUG_CONSILE_TEST)
	ngx_image_thumb_context_t * ctx = (ngx_image_thumb_context_t *)ngx_pcalloc(r->pool, sizeof(ngx_image_thumb_context_t));
	ctx->conf = conf = ngx_http_get_module_loc_conf(r, ngx_http_image_module);
	ctx->r = r;
#else
	//TODO
	char * error;int erroffset;
	ngx_image_thumb_context_t lctx, * ctx = &lctx;
	static ngx_image_conf_t cnf;
	cnf.water_status =1;
	cnf.water_image.len = sizeof("d:/site/pay/test/7.jpg") -1;
	cnf.water_image.data = "d:/site/pay/test/7.jpg";
	cnf.backend = 1;
	cnf.backend_server.data="http://news.baidu.com";
	cnf.backend_server.len = sizeof("http://news.baidu.com") -1;
	
	ctx->conf = conf = &cnf;
	cnf.image_output = 1;
	ctx->r = r;
	cnf.regx_expr = pcre_compile((const char *)"([^<]*)\\/([^<]*)!([a-z])(\\d{2,4})x(\\d{2,4}).([a-zA-Z]{3,4})",0,&error,&erroffset,NULL);
#endif

	if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD)))
	{
		return NGX_HTTP_NOT_ALLOWED;
	}
	if (r->headers_in.if_modified_since)
	{
		return NGX_HTTP_NOT_MODIFIED;
	}
	
	if (r->uri.data[r->uri.len - 1] == '/')
	{
		return NGX_DECLINED;
	}

	rc = ngx_http_discard_request_body(r);
	if (rc != NGX_OK)
	{
		return rc;
	}
	last = ngx_http_map_uri_to_path(r, &ctx->dest_file, &root, 0);

	if (last == NULL)
	{
		return NGX_HTTP_INTERNAL_SERVER_ERROR;
	}

	

	if(file_exists((char*) ctx->dest_file.data) == -1)
	{
		request_uri_len = strlen((char *)r->uri_start) - strlen((char *)r->uri_end);
		request_uri = (u_char *)ngx_alloc( request_uri_len + 1, r->connection->log);
		memcpy(request_uri, r->uri_start, request_uri_len);
		request_uri[request_uri_len]='\0';

		ctx->url = (char *)request_uri;//请求的URL地址

		check_image_type(ctx);//检查图片类型(根据后缀进行简单判断)

		if( ctx->dest_type > 0 )
		{
			if (parse_image_info(ctx) == 0)//解析并处理请求的图片URL
			{
				log_error(NGX_LOG_EMERG, r->connection->log, 0, "LOG: FUCK FUCK FUCK!");
				make_thumb(ctx);//生成图片缩略图
				water_mark(ctx);//图片打上水印
				thumb_to_string(ctx);//GD对象转换成二进制字符串
				if(conf->image_output == 0)
				{
					write_img(ctx);//保存图片缩略图到文件
				}
				if(conf->image_output == 1)
				{
					ngx_free(request_uri);
					return output(r, ctx, ngx_http_image_types[ctx->dest_type]);
				}
			}
		}
		ngx_free(request_uri);
	}
	return NGX_DECLINED;
}

static char * ngx_http_image_water_min(ngx_conf_t *cf, ngx_command_t *cmd,void *conf)
{
	ngx_image_conf_t *info = conf;
	ngx_str_t                         *value;
	ngx_http_complex_value_t           cv;
	ngx_http_compile_complex_value_t   ccv;
	value = cf->args->elts;
	ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
	ccv.cf = cf;
	ccv.value = &value[1];
	ccv.complex_value = &cv;
	if (ngx_http_compile_complex_value(&ccv) != NGX_OK)
	{
		return NGX_CONF_ERROR;
	}
	if (cv.lengths == NULL)
	{
		info->water_width_min = (int)ngx_http_image_value(&value[1]);
		info->water_height_min = (int)ngx_http_image_value(&value[2]);
	}
	return NGX_CONF_OK;
}

static ngx_uint_t ngx_http_image_value(ngx_str_t *value)
{
	ngx_int_t  n;
	if (value->len == 1 && value->data[0] == '-')
	{
		return (ngx_uint_t) -1;
	}
	n = ngx_atoi(value->data, value->len);
	if (n > 0)
	{
		return (ngx_uint_t) n;
	}
	return 0;
}

char * ngx_conf_set_number_slot(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	char  *p = conf;
	int        *np;
	ngx_str_t        *value;
	np = (int *) (p + cmd->offset);
	if (*np != NGX_CONF_UNSET)
	{
		return "is duplicate";
	}
	value = cf->args->elts;
#if !defined(DEBUG_CONSILE_TEST)
	*np = (int)ngx_atoi(value[1].data, value[1].len);
#else
	*np = (int)atoi(value[1].data);
#endif
	if (*np == NGX_ERROR)
	{
		return "invalid number";
	}
	return NGX_CONF_OK;
}

static char *
	ngx_http_image(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_str_t        *value;
	value = cf->args->elts;
	if (ngx_strcasecmp(value[1].data, (u_char *) "on") == 0)
	{
		ngx_http_core_loc_conf_t  *clcf;
		clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
		clcf->handler = ngx_http_image_handler;
		return NGX_CONF_OK;
	}
	else if (ngx_strcasecmp(value[1].data, (u_char *) "off") == 0)
	{
		return NGX_CONF_OK;
	}
	else
	{
		ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,"invalid value \"%s\" in \"%s\" directive, ""it must be \"on\" or \"off\"",value[1].data, cmd->name.data);
		return NGX_CONF_ERROR;
	}
	return NGX_CONF_OK;
}

#if !defined(DEBUG_CONSILE_TEST)
static ngx_int_t output(ngx_http_request_t *r, ngx_image_thumb_context_t  * ctx,ngx_str_t type)
{
    ngx_int_t status = 0;
	ngx_http_complex_value_t  cv;
    ngx_pool_cleanup_t            *cln;
    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        gdFree(ctx->img_data);
        return status;
    }
    cln->handler = gd_clean_data;
    cln->data = ctx->img_data;

	ngx_memzero(&cv, sizeof(ngx_http_complex_value_t));
	cv.value.len = ctx->img_size;
	cv.value.data = (u_char *)ctx->img_data;
	log_error(NGX_LOG_EMERG, r->connection->log, 0, "OUTPUT: %d, %d",ctx->img_data, ctx->img_size);
    status = ngx_http_send_response(r, NGX_HTTP_OK, &type, &cv);
    return status;
}
#endif

static void thumb_to_string(ngx_image_thumb_context_t * ctx)
{
	ngx_image_conf_t *info = ctx->conf;

	switch(ctx->dest_type)
	{
        case NGX_IMAGE_PNG:
            ctx->img_data = (u_char *)gdImagePngPtr(ctx->dst_im,&ctx->img_size);
            break;
        case NGX_IMAGE_GIF:
            ctx->img_data = (u_char *)gdImageGifPtr(ctx->dst_im,&ctx->img_size);
            break;
        case NGX_IMAGE_JPEG:
            ctx->img_data = (u_char *)gdImageJpegPtr(ctx->dst_im, &ctx->img_size,info->jpeg_quality);
            break;
		default:
			ctx->img_data = NULL;
			break;
    }
    gdImageDestroy(ctx->dst_im);
	ctx->dst_im = NULL;
}

static void gd_clean_data(void *data){
	FILE * fp = fopen("D:\\log.txt", "a+");
	if(NULL != data)
	{
		gdFree(data);
		if(NULL != fp){
			fprintf(fp,"FREE GD DATA %X\n", data);
			fclose(fp);
		}
	}
}

static void make_thumb(ngx_image_thumb_context_t  * ctx)
{
	int colors = 0;
	int transparent = -1;

	ctx->dst_im = gdImageCreateTrueColor(ctx->width,ctx->height);
	colors = gdImageColorsTotal(ctx->src_im);
	transparent = gdImageGetTransparent(ctx->src_im);
	if (transparent == -1)
        {
		gdImageSaveAlpha(ctx->src_im,1);
		gdImageColorTransparent(ctx->src_im, -1);
		if(colors == 0)
		{
			gdImageAlphaBlending(ctx->dst_im,0);
			gdImageSaveAlpha(ctx->dst_im,1);
		}
		if(colors)
		{
			gdImageTrueColorToPalette(ctx->dst_im,1,256);
		}
    }
    if(ctx->w_margin == 1)
    {
		ctx->w_im = gdImageCreateTrueColor(ctx->width,ctx->height);
		gdImageFilledRectangle(ctx->w_im, 0, 0, ctx->width,ctx->height, gdImageColorAllocate(ctx->w_im, 255, 255, 255));

        if(NULL != ctx->dst_im){
			gdImageDestroy(ctx->dst_im);
		}
		ctx->dst_im = gdImageCreateTrueColor(ctx->max_width,ctx->max_height);

        gdImageFilledRectangle(ctx->dst_im, 0, 0, ctx->max_width,ctx->max_height, gdImageColorAllocate(ctx->dst_im, 255, 255, 255));
		gdImageCopyResampled(ctx->w_im, ctx->src_im, 0, 0, ctx->src_x, ctx->src_y,ctx->width, ctx->height, ctx->src_w,ctx->src_h);
		gdImageCopyResampled(ctx->dst_im, ctx->w_im, ctx->dst_x,ctx->dst_y, 0, 0,ctx->width, ctx->height, ctx->width, ctx->height);
        gdImageDestroy(ctx->w_im);
		ctx->w_im = NULL;
    }
    else
    {

        gdImageCopyResampled(ctx->dst_im,ctx->src_im,ctx->dst_x,ctx->dst_y,ctx->src_x,ctx->src_y,ctx->width,ctx->height,ctx->src_w,ctx->src_h);
    }
    gdImageDestroy(ctx->src_im);
	ctx->src_im = NULL;
}
static void water_mark(ngx_image_thumb_context_t * ctx)
{
	ngx_image_conf_t *info = ctx->conf;
	int water_w=0;//水印宽度
	int water_h=0;//水印高度
	int posX = 0;//X位置
	int posY = 0;//Y位置
	int water_color = 0;//文字水印GD颜色值
	char *water_text;//图片文字
	char *water_font;//文字字体
	char *water_color_text;//图片颜色值
	water_text = NULL;
	water_font = NULL;
	water_color_text = NULL;

	if(info->water_status)//如果水印功能打开了
	{

		if(info->water_type == 0)//如果为图片水印
		{
			if(file_exists((char *)info->water_image.data) == 0)//判断水印图片是否存在
			{
				water_image_from(ctx);//获取水印图片信息
				if(ctx->water_im == NULL)//判断对象是否为空
				{
                    return;//水印文件异常
                }else{
                    water_w = ctx->water_im->sx;
                    water_h = ctx->water_im->sy;
                }
			}
			else
			{
				return;//水印图片不存在
			}
		}
		else//文字水印
		{
			water_text = (char *) info->water_text.data;
			water_color_text = (char *) info->water_color.data;
			water_font = (char *)info->water_font.data;
			if(file_exists((char *)water_font) == 0)//如果水印字体存在
			{
				int R,G,B;
				char R_str[3],G_str[3],B_str[3];
				int brect[8];
				gdImagePtr font_im;
				font_im = gdImageCreateTrueColor(ctx->dst_im->sx,ctx->dst_im->sy);
				sprintf(R_str,"%.*s",2,water_color_text+1);
				sprintf(G_str,"%.*s",2,water_color_text+3);
				sprintf(B_str,"%.*s",2,water_color_text+5);
				sscanf(R_str,"%x",&R);
				sscanf(G_str,"%x",&G);
				sscanf(B_str,"%x",&B);
				water_color = gdImageColorAllocate(ctx->dst_im,R,G,B);
				gdImageStringFT(font_im, &brect[0], water_color, water_font, info->water_font_size, 0.0, 0, 0,water_text/*, &strex*/);
				//water_w = abs(brect[2] - brect[6] + 10);
				water_w = abs(brect[2] - brect[6] + 10);
				water_h = abs(brect[3] - brect[7]);
				gdImageDestroy(font_im);
			}

		}
		if( (ctx->width < info->water_width_min) || ctx->height < info->water_height_min)
		{
			return;//如果图片宽度/高度比配置文件里规定的宽度/高度宽度小
		}
		if ((ctx->width < water_w) || (ctx->height < water_h))
		{
			return;//如果图片宽度/高度比水印宽度/高度宽度小
		}
		if(info->water_pos < 1 ||info->water_pos > 9)
		{
			srand((unsigned)time(NULL));
			//info->water_pos = rand() % 9 + 1;
			info->water_pos = 1+(int)(9.0*rand()/(RAND_MAX+1.0));
			//info->water_pos = rand() % 9;
		}
		switch(info->water_pos)
		{
		case 1:
			posX = 10;
			posY = 15;
			break;
		case 2:
			posX = (ctx->width - water_w) / 2;
			posY = 15;
			break;
		case 3:
			posX = ctx->width - water_w;
			posY = 15;
			break;
		case 4:
			posX = 0;
			posY = (ctx->height - water_h) / 2;
			break;
		case 5:
			posX = (ctx->width - water_w) / 2;
			posY = (ctx->height - water_h) / 2;
			break;
		case 6:
			posX = ctx->width - water_w;
			posY = (ctx->height - water_h) / 2;
			break;
		case 7:
			posX = 0;
			posY = (ctx->height - water_h);
			break;
		case 8:
			posX = (ctx->width - water_w) /2;
			posY = ctx->width - water_h;
			break;
		case 9:
			posX = ctx->width - water_w;
			posY = ctx->height - water_h;
			break;
		default:
			posX = ctx->width - water_w;
			posY = ctx->height - water_h;
			break;
		}
		if(info->water_type == 0)
		{
			gdImagePtr tmp_im;
			tmp_im = NULL;
			tmp_im = gdImageCreateTrueColor(water_w, water_h);
			gdImageCopy(tmp_im, ctx->dst_im, 0, 0, posX, posY, water_w, water_h);
			gdImageCopy(tmp_im, ctx->water_im, 0, 0, 0, 0, water_w, water_h);
			gdImageCopyMerge(ctx->dst_im, tmp_im,posX, posY, 0, 0, water_w,water_h,info->water_transparent);
			gdImageDestroy(tmp_im);
            gdImageDestroy(ctx->water_im);
			ctx->water_im = NULL;
		}
		else
		{
			gdImageAlphaBlending(ctx->dst_im,-1);
			gdImageSaveAlpha(ctx->dst_im,0);
			gdImageStringFT(ctx->dst_im,0,water_color,water_font,info->water_font_size, 0.0, posX, posY,water_text);
		}
	}
}

static int parse_image_info(ngx_image_thumb_context_t * ctx)
{
    char buffer[7][255];
	const char *error;//正则错误内容
	int pcre_state=0;//匹配图片规则状态,0为成功 -1为失败
	int erroffset;//正则错误位置
	int ovector[30];//识别器读取原图图片到GD对象
	int expr_res;//正则匹配指针
	int i=0;//循环用
	ngx_str_t dest_file;
	ngx_image_conf_t *info = ctx->conf;
	ctx->request_filename = NULL;

#if defined(DEBUG_CONSILE_TEST)
	expr_res = pcre_exec(ctx->conf->regx_expr,NULL,(const char *)ctx->dest_file.data, ctx->dest_file.len-1,0,0,ovector,sizeof(ovector)/sizeof(ovector[0]));
#else
	expr_res = ngx_regex_exec(ctx->conf->regx_expr, &ctx->dest_file,ovector, sizeof(ovector)/sizeof(ovector[0]));
#endif

	if(expr_res > 5)
	{
		for(i=0; i<expr_res; i++)
		{
			char *substring_start = ctx->dest_file.data + ovector[2*i];
			int substring_length = ovector[2*i+1] - ovector[2*i];
			if(substring_length > 254){
				return -1;
			}
			sprintf(buffer[i],"%.*s",substring_length,substring_start);
			//printf("%d : %.*s\n",i,substring_length,substring_start);
		}
		ctx->source_file = buffer[1];
		/** combind source_file **/
		strcat(ctx->source_file,"/");
		strcat(ctx->source_file,buffer[2]);
		/** combind request_filename **/
		ctx->request_filename = buffer[2];


		ctx->dest_file.data = (u_char *)buffer[0];
		ctx->dest_file.len = ovector[1]-ovector[0];

		info->m_type = buffer[3];
		ctx->max_width = atoi(buffer[4]);
		ctx->max_height = atoi(buffer[5]);
		ctx->max_width = (ctx->max_width > 2000) ? 2000 : ctx->max_width;
		ctx->max_height = (ctx->max_height > 2000) ? 2000 : ctx->max_height;
		if(ctx->max_width <= 0 || ctx->max_height <=0 ){
                        //如果图片小于等于0，则可以判断请求无效了
                        return -1;
                    }
		//printf("source_file:%s\n",ctx->source_file);
		if(0 != info->backend && file_exists(ctx->source_file) == -1)//原图不存在
		{
			if(0 < dirname((char *)buffer[1],&ctx->local_dir)){
				download(ctx);
				free(ctx->local_dir);
				ctx->local_dir = NULL;
			}
		}

		if(file_exists(ctx->source_file) == 0)
		{
			pcre_state = calc_image_info(ctx);
			return pcre_state;
		}
	}
	return -1;
}
static int calc_image_info(ngx_image_thumb_context_t * ctx)
{
	ngx_image_conf_t *info = ctx->conf;

	ctx->src_type = get_ext_header(ctx->source_file);//读取原图头部信息金星判断图片格式
	if( ctx->src_type > 0)
	{
        ctx->w_margin = 0;//设置默认图片不补白边

		image_from(ctx);//读取原图图片到GD对象
		if(ctx->src_im != NULL)
		{

			ctx->src_width = ctx->src_im->sx;
			ctx->src_height = ctx->src_im->sy;
			ctx->src_x = 0;
			ctx->src_y = 0;
            ctx->dst_x = 0;
            ctx->dst_y = 0;
			ctx->src_w = ctx->src_width;
			ctx->src_h = ctx->src_height;
			ctx->width = ctx->max_width;
			ctx->height = ctx->max_height;
			if(stricmp(info->m_type,"c") == 0 || stricmp(info->m_type,"m") == 0)
			{
				if((double)ctx->src_width/ctx->src_height > (double)ctx->max_width / ctx->max_height)
				{
					ctx->src_w=ctx->width * ctx->src_height / ctx->height;
					if(stricmp(info->m_type,"m") == 0)
					{
						ctx->src_x=(ctx->src_width-ctx->src_w)/2;
					}
					else
					{
						ctx->src_x=(ctx->src_width-ctx->src_w)*0.1;
					}
				}
				else
				{
					ctx->src_h=ctx->src_w * ctx->height / ctx->width;
					if(stricmp(info->m_type,"m") == 0)
					{
						ctx->src_y=(ctx->src_height-ctx->src_h)/2;
					}
					else
					{
						ctx->src_y=(ctx->src_height-ctx->src_h)*0.1;
					}
				}
			}
			else if(stricmp(info->m_type,"t") == 0)
			{
				if( (ctx->max_width >= ctx->src_width) || (ctx->max_height >= ctx->src_height ))
				{
					ctx->max_width = ctx->src_width;
					ctx->max_height = ctx->src_height;
					ctx->height = ctx->src_height;
					ctx->width = ctx->src_width;
				}
				else
				{
					if((double)ctx->src_width/ctx->src_height >= (double) ctx->max_width / ctx->max_height)
					{
						ctx->height=ctx->width * ctx->src_height/ctx->src_width;
						ctx->src_h=ctx->src_w * ctx->height / ctx->width;
						ctx->src_y=(ctx->src_height - ctx->src_h) / 2;
					}
					else
					{
						ctx->width=ctx->max_height * ctx->src_width / ctx->src_height;
						ctx->src_w=ctx->width * ctx->src_height / ctx->height;
						ctx->src_x=(ctx->src_width - ctx->src_w)/2;
					}
				}
			}
			else if(stricmp(info->m_type,"w") == 0)
			{
				ctx->w_margin = 1;
				if((double)ctx->src_width/ctx->src_height >= (double) ctx->max_width / ctx->max_height)
				{
					ctx->height=ctx->width * ctx->src_height/ctx->src_width;
					ctx->src_h=ctx->src_w * ctx->height / ctx->width;
					ctx->src_y=(ctx->src_height - ctx->src_h) / 2;
				}
				else
				{
					ctx->width=ctx->max_height * ctx->src_width / ctx->src_height;
					ctx->src_w=ctx->width * ctx->src_height / ctx->height;
					ctx->src_x=(ctx->src_width - ctx->src_w)/2;
				}
                ctx->dst_x = (float)((float)(ctx->max_width - ctx->width)/2);
                ctx->dst_y = (float)((float)(ctx->max_height - ctx->height)/2);
                		
			}
			else
			{
				return -1;
			}
			return 0;
		}
	}
	return -1;
}

static void check_image_type(ngx_image_thumb_context_t * ctx)
{
	ngx_image_conf_t *info = ctx->conf;
	info->extension = get_ext(ctx->dest_file.data);
	if(stricmp(info->extension,"jpg") == 0 || stricmp(info->extension,"jpeg") == 0 || stricmp(info->extension,"jpe") == 0)
	{
		ctx->dest_type = NGX_IMAGE_JPEG;
	}
	else if(stricmp(info->extension,"png") == 0)
	{
		ctx->dest_type = NGX_IMAGE_PNG;
	}
	else if(stricmp(info->extension,"gif") == 0)
	{
		ctx->dest_type = NGX_IMAGE_GIF;
	}
	else
	{
		ctx->dest_type = NGX_IMAGE_NONE;
	}
}

static char * get_ext(char *filename)
{
	char *p = strrchr(filename, '.');
	if(p != NULL)
	{
		return p+1;
	}
	else
	{
		return filename;
	}
}

static int file_exists(char *filename)
{
	if(NULL != filename && access(filename, 0) != -1)
		return 0;
	return -1;
}

static int get_ext_header(char *filename)
{
	FILE * handle = fopen(filename, "rb");
	unsigned short filetype;//bmp 0x4D42
	int r = NGX_IMAGE_NONE;

	if(handle)
	{
		if(fread(&filetype,sizeof(unsigned short),1,handle) == 1)
		{
			switch(filetype){
			case 0xD8FF:
				r = NGX_IMAGE_JPEG;break;
			case 0x4947:
				r = NGX_IMAGE_GIF;break;
			case 0x5089:
				r = NGX_IMAGE_PNG;break;
			}
		}
		fclose(handle);
	}
	return r;
}

static void write_img(ngx_image_thumb_context_t * ctx)
{
	if(ctx->img_data != NULL)
	{
		FILE * fp;
		if(NULL != (fp = fopen(ctx->dest_file.data,"wb"))){
			fwrite(ctx->img_data,sizeof(char),ctx->img_size,fp);
			fclose(fp);
		}
	}
	gdFree(ctx->img_data);
	ctx->img_data = NULL;
	ctx->img_size = 0;
}

static gdImagePtr read_image(const char * filename, int * ftype){
	FILE * handle = fopen(filename, "rb");
	gdImagePtr r = NULL;
	if(NULL != handle){
		unsigned short filetype = 0;
		if(fread(&filetype,sizeof(unsigned short),1,handle) == 1){
			fseek(handle, 0, SEEK_SET);
			switch(filetype){
			case 0xD8FF:
				*ftype = filetype = NGX_IMAGE_JPEG;
				r = gdImageCreateFromJpeg(handle);
				break;
			case 0x4947:
				*ftype = filetype = NGX_IMAGE_GIF;
				r = gdImageCreateFromGif(handle);
				break;
			case 0x5089:
				*ftype = filetype = NGX_IMAGE_PNG;
				r = gdImageCreateFromPng(handle);
				break;
			default:
				*ftype = filetype = 0;
			}
		}
		fclose(handle);
	}
	return r;
}



static void water_image_from(ngx_image_thumb_context_t * ctx)
{
	ngx_image_conf_t *info = ctx->conf;
	ctx->water_im_type = get_ext_header((char *)info->water_image.data);

	if(NGX_IMAGE_NONE != ctx->water_im_type){
		char * buffer;
		size_t buffer_size;
		buffer = get_file_contents((char *)info->water_image.data, &buffer_size);
		if(NULL != buffer){
			switch(ctx->water_im_type)
			{
			case NGX_IMAGE_GIF:
				ctx->water_im = gdImageCreateFromGifPtr(buffer_size,buffer);
				break;
			case NGX_IMAGE_JPEG:
				ctx->water_im = gdImageCreateFromJpegPtr(buffer_size,buffer);
				break;
			case NGX_IMAGE_PNG:
				ctx->water_im = gdImageCreateFromPngPtr(buffer_size,buffer);
				break;
			}
			free(buffer);
		}
	}
}

static char * get_file_contents(const char* file_name, size_t * len)
{
	FILE * fp = fopen(file_name,"rb");
	if(NULL != fp)
	{
		int filesize;size_t rlen;
		fseek(fp, 0, SEEK_END);
		filesize = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		if(filesize > 0 )
		{
			char * data = (char *)malloc(filesize+1);
			rlen = fread(data, 1, filesize, fp);
			data[rlen] = '\0';
			* len = filesize;
			fclose(fp);
			return data;
		}
		fclose(fp);
	}
	*len =0;
	return NULL;
}

static void image_from(ngx_image_thumb_context_t * ctx)
{
	FILE * fp;
	ngx_image_conf_t *info = ctx->conf;
	size_t buffer_size;
//	ctx->src_im = read_image(ctx->conf->source_file, ctx->src_type);
	char * buffer = get_file_contents(ctx->source_file, &buffer_size);

	if(NULL != buffer){
		switch(ctx->src_type)
		{
		case NGX_IMAGE_GIF:
			ctx->src_im = gdImageCreateFromGifPtr(buffer_size, buffer);
			break;
		case NGX_IMAGE_JPEG:
			ctx->src_im = gdImageCreateFromJpegPtr(buffer_size, buffer);
			break;
		case NGX_IMAGE_PNG:
			ctx->src_im = gdImageCreateFromPngPtr(buffer_size, buffer);
			break;
		}
		free(buffer);
	}

	return;
}

static int get_header(char *url)
{
	long result = 0;
	CURL *curl;
	CURLcode status;

	curl = curl_easy_init();

	memset(&status,0,sizeof(CURLcode));
	if(curl)
	{
		curl_easy_setopt(curl, CURLOPT_URL,url);
		curl_easy_setopt(curl, CURLOPT_NOBODY, 1);
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST,"GET");
		status = curl_easy_perform(curl);
		if(status != CURLE_OK)
		{
			curl_easy_cleanup(curl);
			return -1;
		}
		status = curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&result);
		if((status == CURLE_OK) && result == 200)
		{
			curl_easy_cleanup(curl);
			return 0;
		}
		curl_easy_cleanup(curl);
		return -1;
	}
	return -1;
}

static size_t curl_get_data(void *ptr, size_t size, size_t nmemb, void *stream)
{
	int written = fwrite (ptr, size, nmemb, (FILE *) curl_handle);
	return written;
}

static int dirname(char *path,char **dirpath)
{
	automem_t mem;
	int len = strlen(path);
	mem.pdata = NULL;

	//从最后一个元素开始找.直到找到第一个'/'
	while(0 < len--){
		if(path[len] == '/'){
			automem_init(&mem, len+1);
			strncpy((char *)mem.pdata, path, len+1);
			mem.pdata[len] = '\0';
			break;
		}
	}
	if(len > 0)
		*dirpath = (char *)mem.pdata;

	return len;
}

static int create_dir(char *dir)
{
	int i;
	int len;
	char dirname[256];
	
	strcpy(dirname,dir);
	len=strlen(dirname);

	if(dirname[len-1]!='/')
	{
		strcat(dirname,"/");
	}
	len=strlen(dirname);
	for(i=1; i<len; i++)
	{
		if(dirname[i]=='/')
		{
			dirname[i] = 0;
			if(access(dirname,0)!=0)
			{
				if(mkdir(dirname,0777)==-1)
				{
					return -1;
				}
			}
			dirname[i] = '/';
		}
	}
	return 0;
}

static void get_request_source(ngx_image_thumb_context_t * ctx)
{
	ngx_image_conf_t *info = ctx->conf;
	automem_t mem;
	automem_init(&mem, 512);
	automem_append_voidp(&mem,info->backend_server.data, info->backend_server.len);
	automem_append_voidp(&mem, ctx->request_dir, strlen(ctx->request_dir));
	automem_append_byte(&mem,'/');
	automem_append_voidp(&mem, ctx->request_filename, strlen(ctx->request_filename));
	automem_append_byte(&mem,'\0');
	ctx->request_source = (char *)mem.pdata;
}


static void download(ngx_image_thumb_context_t * ctx)
{
	ngx_image_conf_t *info = ctx->conf;

	if(NULL == info->backend_server.data){
		//backend_server 没设置!
		return;
	}
	if(0 < dirname(ctx->url, &ctx->request_dir))
	{
		get_request_source(ctx);//取得请求的URL的原始文件
		free(ctx->request_dir);
		if (get_header(ctx->request_source) == 0)
		{
			CURL *curl = curl_easy_init();
			if(NULL != curl){
				create_dir(ctx->local_dir);//创建目录
				if((curl_handle = fopen(ctx->source_file, "wb")) == NULL)
				{
					curl_easy_cleanup (curl);
					fclose(curl_handle);
					curl_handle = NULL;
					free(ctx->request_source);
					return;
				}
				if(curl)
				{
					curl_easy_setopt(curl, CURLOPT_URL,ctx->request_source);
					curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_get_data);
					curl_easy_perform(curl);
					curl_easy_cleanup(curl);
					fclose(curl_handle);
					curl_handle = NULL;
				}
			}
		}
		free(ctx->request_source);
		ctx->request_source=NULL;
	}
}




#if defined(DEBUG_CONSILE_TEST)

int main(int argc, char * argv[])
{
	pcre *expr;//正则
	ngx_connection_t  ngx_conn;
	const char *error;int erroffset;//正则错误位置
	ngx_http_request_t req;
	req.http_version = NGX_HTTP_VERSION_11;
	req.request_line.len = sizeof("GET /");
	req.request_line.data = "GET /";
	req.headers_in.if_modified_since= 0;
	
	req.method=NGX_HTTP_GET;
	req.uri_start = req.uri.data=REQUEST_URI_CONST;
	req.uri.len = sizeof(REQUEST_URI_CONST);
	req.uri_end = req.uri_start + sizeof(REQUEST_URI_CONST)-1;
	ngx_conn.log = NULL;
	req.connection = &ngx_conn;
	//req.

	ngx_http_image_handler(&req);
	_CrtDumpMemoryLeaks();

	return 0;
}

#endif
