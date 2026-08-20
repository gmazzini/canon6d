// Gianluca Mazzini @2026- Version 1.00

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define PTPIP_INIT_COMMAND_REQUEST 0x00000001U
#define PTPIP_INIT_COMMAND_ACK 0x00000002U
#define PTPIP_INIT_EVENT_REQUEST 0x00000003U
#define PTPIP_INIT_EVENT_ACK 0x00000004U
#define PTPIP_COMMAND_REQUEST 0x00000006U
#define PTPIP_COMMAND_RESPONSE 0x00000007U
#define PTPIP_DATA_START 0x00000009U
#define PTPIP_DATA 0x0000000aU
#define PTPIP_DATA_END 0x0000000cU

#define PTPIP_DATA_DIR_CAMERA_TO_HOST_OR_NONE 0x00000001U

#define PTP_OP_OPEN_SESSION 0x1002U
#define PTP_OP_CLOSE_SESSION 0x1003U
#define PTP_OP_GET_STORAGE_IDS 0x1004U
#define PTP_OP_GET_OBJECT_HANDLES 0x1007U
#define PTP_OP_GET_OBJECT_INFO 0x1008U
#define PTP_OP_GET_PARTIAL_OBJECT 0x101bU

#define PTP_RESP_OK 0x2001U
#define PTP_FORMAT_ASSOCIATION 0x3001U

#define TRANSFER_CHUNK_SIZE 1048576U
#define MAX_MTP_DATA_SIZE 67108864U
#define MAX_FRAME_SIZE (MAX_MTP_DATA_SIZE + 64U)
#define FILENAME_SIZE 1024U
#define SOCKET_TIMEOUT_SECONDS 15

typedef struct {
  int command_socket;
  int event_socket;
  unsigned int connection_id;
  unsigned int transaction_id;
} Camera;

typedef struct {
  unsigned int size;
  unsigned short format;
  char filename[FILENAME_SIZE];
} ObjectInfo;

static void put_u16le(unsigned char *p,unsigned short v) {
  p[0]=(unsigned char)(v&0xffU);
  p[1]=(unsigned char)((v>>8)&0xffU);
}

static void put_u32le(unsigned char *p,unsigned int v) {
  p[0]=(unsigned char)(v&0xffU);
  p[1]=(unsigned char)((v>>8)&0xffU);
  p[2]=(unsigned char)((v>>16)&0xffU);
  p[3]=(unsigned char)((v>>24)&0xffU);
}

static unsigned short get_u16le(const unsigned char *p) {
  unsigned short v;

  v=(unsigned short)p[0];
  v|=(unsigned short)((unsigned short)p[1]<<8);
  return v;
}

static unsigned int get_u32le(const unsigned char *p) {
  unsigned int v;

  v=(unsigned int)p[0];
  v|=(unsigned int)p[1]<<8;
  v|=(unsigned int)p[2]<<16;
  v|=(unsigned int)p[3]<<24;
  return v;
}

static int send_all(int s,const unsigned char *data,unsigned int len) {
  unsigned int done;
  ssize_t n;

  done=0;
  for(;done<len;) {
    n=send(s,data+done,(size_t)(len-done),0);
    if(n<0) {
      if(errno==EINTR)
        continue;
      return -1;
    }
    if(n==0)
      return -1;
    done+=(unsigned int)n;
  }
  return 0;
}

static int recv_all(int s,unsigned char *data,unsigned int len) {
  unsigned int done;
  ssize_t n;

  done=0;
  for(;done<len;) {
    n=recv(s,data+done,(size_t)(len-done),0);
    if(n<0) {
      if(errno==EINTR)
        continue;
      return -1;
    }
    if(n==0)
      return -1;
    done+=(unsigned int)n;
  }
  return 0;
}

static int send_frame(int s,const unsigned char *payload,unsigned int payload_len) {
  unsigned char header[4];

  if(payload_len>MAX_FRAME_SIZE-4U) {
    errno=EMSGSIZE;
    return -1;
  }
  put_u32le(header,payload_len+4U);
  if(send_all(s,header,4U)<0)
    return -1;
  return send_all(s,payload,payload_len);
}

static int recv_frame(int s,unsigned char **payload,unsigned int *payload_len) {
  unsigned char header[4];
  unsigned char *data;
  unsigned int total_len;
  unsigned int len;

  *payload=NULL;
  *payload_len=0;
  if(recv_all(s,header,4U)<0)
    return -1;
  total_len=get_u32le(header);
  if(total_len<8U || total_len>MAX_FRAME_SIZE) {
    errno=EPROTO;
    return -1;
  }
  len=total_len-4U;
  data=(unsigned char *)malloc((size_t)len);
  if(data==NULL) {
    errno=ENOMEM;
    return -1;
  }
  if(recv_all(s,data,len)<0) {
    free(data);
    return -1;
  }
  *payload=data;
  *payload_len=len;
  return 0;
}

static int open_ipv4_socket(const char *ip,unsigned short port) {
  struct sockaddr_in addr;
  struct timeval timeout;
  int s;

  memset(&addr,0,sizeof(addr));
  addr.sin_family=AF_INET;
  addr.sin_port=htons(port);
  if(inet_pton(AF_INET,ip,&addr.sin_addr)!=1) {
    errno=EINVAL;
    return -1;
  }

  s=socket(AF_INET,SOCK_STREAM,0);
  if(s<0)
    return -1;

  timeout.tv_sec=SOCKET_TIMEOUT_SECONDS;
  timeout.tv_usec=0;
  if(setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))<0 ||
     setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))<0) {
    close(s);
    return -1;
  }

  if(connect(s,(struct sockaddr *)&addr,sizeof(addr))<0) {
    close(s);
    return -1;
  }
  return s;
}

static int init_command_channel(Camera *camera,const char *ip,unsigned short port) {
  static const unsigned char guid[16]={
    0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
    0x88,0x99,0x00,0x00,0x00,0x00,0x00,0x00
  };
  static const char host_name[]="airmtp";
  unsigned char request[64];
  unsigned char *response;
  unsigned int response_len;
  unsigned int pos;
  unsigned int i;

  camera->command_socket=open_ipv4_socket(ip,port);
  if(camera->command_socket<0)
    return -1;

  put_u32le(request,PTPIP_INIT_COMMAND_REQUEST);
  memcpy(request+4,guid,sizeof(guid));
  pos=20U;
  for(i=0U;host_name[i]!='\0';i++) {
    request[pos++]=(unsigned char)host_name[i];
    request[pos++]=0;
  }
  request[pos++]=0;
  request[pos++]=0;
  put_u32le(request+pos,0x00010000U);
  pos+=4U;

  if(send_frame(camera->command_socket,request,pos)<0)
    return -1;
  if(recv_frame(camera->command_socket,&response,&response_len)<0)
    return -1;
  if(response_len<8U || get_u32le(response)!=PTPIP_INIT_COMMAND_ACK) {
    free(response);
    errno=EPROTO;
    return -1;
  }

  camera->connection_id=get_u32le(response+4);
  free(response);
  return 0;
}

static int init_event_channel(Camera *camera,const char *ip,unsigned short port) {
  unsigned char request[8];
  unsigned char *response;
  unsigned int response_len;

  camera->event_socket=open_ipv4_socket(ip,port);
  if(camera->event_socket<0)
    return -1;

  put_u32le(request,PTPIP_INIT_EVENT_REQUEST);
  put_u32le(request+4,camera->connection_id);
  if(send_frame(camera->event_socket,request,sizeof(request))<0)
    return -1;
  if(recv_frame(camera->event_socket,&response,&response_len)<0)
    return -1;
  if(response_len<4U || get_u32le(response)!=PTPIP_INIT_EVENT_ACK) {
    free(response);
    errno=EPROTO;
    return -1;
  }

  free(response);
  return 0;
}

static int mtp_exec(Camera *camera,unsigned short operation,
                    const unsigned int *params,unsigned int param_count,
                    unsigned char **result_data,unsigned int *result_len,
                    unsigned int *response_param) {
  unsigned char request[34];
  unsigned char *frame;
  unsigned char *data;
  unsigned char *new_data;
  unsigned int request_len;
  unsigned int frame_len;
  unsigned int frame_type;
  unsigned int transaction_id;
  unsigned int rx_transaction_id;
  unsigned int expected_len;
  unsigned int data_len;
  unsigned int capacity;
  unsigned int chunk_len;
  unsigned int i;
  unsigned short response_code;

  if(param_count>5U) {
    errno=EINVAL;
    return -1;
  }

  *result_data=NULL;
  *result_len=0U;
  if(response_param!=NULL)
    *response_param=0U;

  camera->transaction_id++;
  transaction_id=camera->transaction_id;

  put_u32le(request,PTPIP_COMMAND_REQUEST);
  put_u32le(request+4,PTPIP_DATA_DIR_CAMERA_TO_HOST_OR_NONE);
  put_u16le(request+8,operation);
  put_u32le(request+10,transaction_id);
  request_len=14U;
  for(i=0U;i<param_count;i++) {
    put_u32le(request+request_len,params[i]);
    request_len+=4U;
  }

  if(send_frame(camera->command_socket,request,request_len)<0)
    return -1;

  data=NULL;
  data_len=0U;
  capacity=0U;
  expected_len=0U;

  for(;;) {
    frame=NULL;
    frame_len=0U;
    if(recv_frame(camera->command_socket,&frame,&frame_len)<0) {
      free(data);
      return -1;
    }

    frame_type=get_u32le(frame);
    if(frame_type==PTPIP_DATA_START) {
      if(frame_len<12U) {
        free(frame);
        free(data);
        errno=EPROTO;
        return -1;
      }
      rx_transaction_id=get_u32le(frame+4);
      expected_len=get_u32le(frame+8);
      free(frame);
      if(rx_transaction_id!=transaction_id || expected_len>MAX_MTP_DATA_SIZE) {
        free(data);
        errno=EPROTO;
        return -1;
      }
      if(expected_len>capacity) {
        new_data=(unsigned char *)realloc(data,(size_t)expected_len);
        if(new_data==NULL && expected_len!=0U) {
          free(data);
          errno=ENOMEM;
          return -1;
        }
        data=new_data;
        capacity=expected_len;
      }
      continue;
    }

    if(frame_type==PTPIP_DATA || frame_type==PTPIP_DATA_END) {
      if(frame_len<8U) {
        free(frame);
        free(data);
        errno=EPROTO;
        return -1;
      }
      rx_transaction_id=get_u32le(frame+4);
      chunk_len=frame_len-8U;
      if(rx_transaction_id!=transaction_id ||
         data_len>MAX_MTP_DATA_SIZE-chunk_len) {
        free(frame);
        free(data);
        errno=EPROTO;
        return -1;
      }
      if(data_len+chunk_len>capacity) {
        capacity=data_len+chunk_len;
        new_data=(unsigned char *)realloc(data,(size_t)capacity);
        if(new_data==NULL && capacity!=0U) {
          free(frame);
          free(data);
          errno=ENOMEM;
          return -1;
        }
        data=new_data;
      }
      if(chunk_len!=0U)
        memcpy(data+data_len,frame+8,chunk_len);
      data_len+=chunk_len;
      free(frame);
      continue;
    }

    if(frame_type==PTPIP_COMMAND_RESPONSE) {
      if(frame_len<10U) {
        free(frame);
        free(data);
        errno=EPROTO;
        return -1;
      }
      response_code=get_u16le(frame+4);
      rx_transaction_id=get_u32le(frame+6);
      if(response_param!=NULL && frame_len>=14U)
        *response_param=get_u32le(frame+10);
      free(frame);

      if(rx_transaction_id!=transaction_id) {
        free(data);
        errno=EPROTO;
        return -1;
      }
      if(response_code!=PTP_RESP_OK) {
        free(data);
        fprintf(stderr,"PTP operation 0x%04x failed: response 0x%04x\n",
                (unsigned int)operation,(unsigned int)response_code);
        errno=EIO;
        return -1;
      }
      if(expected_len!=0U && data_len!=expected_len) {
        free(data);
        errno=EPROTO;
        return -1;
      }

      *result_data=data;
      *result_len=data_len;
      return 0;
    }

    free(frame);
    free(data);
    errno=EPROTO;
    return -1;
  }
}

static int open_session(Camera *camera) {
  unsigned int params[1];
  unsigned char *data;
  unsigned int data_len;

  params[0]=camera->connection_id;
  data=NULL;
  data_len=0U;
  if(mtp_exec(camera,PTP_OP_OPEN_SESSION,params,1U,&data,&data_len,NULL)<0)
    return -1;
  free(data);
  return 0;
}

static void close_session(Camera *camera) {
  unsigned char *data;
  unsigned int data_len;

  data=NULL;
  data_len=0U;
  if(camera->command_socket>=0) {
    if(mtp_exec(camera,PTP_OP_CLOSE_SESSION,NULL,0U,&data,&data_len,NULL)==0)
      free(data);
  }
}

static int get_storage_ids(Camera *camera,unsigned int **ids,unsigned int *count) {
  unsigned char *data;
  unsigned int data_len;
  unsigned int n;
  unsigned int i;
  unsigned int *list;

  *ids=NULL;
  *count=0U;
  data=NULL;
  data_len=0U;

  if(mtp_exec(camera,PTP_OP_GET_STORAGE_IDS,NULL,0U,&data,&data_len,NULL)<0)
    return -1;
  if(data_len<4U) {
    free(data);
    errno=EPROTO;
    return -1;
  }

  n=get_u32le(data);
  if(n>(data_len-4U)/4U) {
    free(data);
    errno=EPROTO;
    return -1;
  }

  list=NULL;
  if(n!=0U) {
    list=(unsigned int *)malloc((size_t)n*sizeof(unsigned int));
    if(list==NULL) {
      free(data);
      errno=ENOMEM;
      return -1;
    }
    for(i=0U;i<n;i++)
      list[i]=get_u32le(data+4U+i*4U);
  }

  free(data);
  *ids=list;
  *count=n;
  return 0;
}

static int get_object_handles(Camera *camera,unsigned int storage_id,
                              unsigned int **handles,unsigned int *count) {
  unsigned int params[3];
  unsigned char *data;
  unsigned int data_len;
  unsigned int n;
  unsigned int i;
  unsigned int *list;

  *handles=NULL;
  *count=0U;
  params[0]=storage_id;
  params[1]=0U;
  params[2]=0xffffffffU;
  data=NULL;
  data_len=0U;

  if(mtp_exec(camera,PTP_OP_GET_OBJECT_HANDLES,params,3U,
              &data,&data_len,NULL)<0)
    return -1;
  if(data_len<4U) {
    free(data);
    errno=EPROTO;
    return -1;
  }

  n=get_u32le(data);
  if(n>(data_len-4U)/4U) {
    free(data);
    errno=EPROTO;
    return -1;
  }

  list=NULL;
  if(n!=0U) {
    list=(unsigned int *)malloc((size_t)n*sizeof(unsigned int));
    if(list==NULL) {
      free(data);
      errno=ENOMEM;
      return -1;
    }
    for(i=0U;i<n;i++)
      list[i]=get_u32le(data+4U+i*4U);
  }

  free(data);
  *handles=list;
  *count=n;
  return 0;
}

static int mtp_string_to_utf8(const unsigned char *data,unsigned int len,
                              char *out,unsigned int out_size) {
  unsigned int count;
  unsigned int i;
  unsigned int pos;
  unsigned int cp;
  unsigned int w1;
  unsigned int w2;

  if(len<1U || out_size<1U)
    return -1;

  count=(unsigned int)data[0];
  if(count==0U) {
    out[0]='\0';
    return 0;
  }
  if(count>255U || len<1U+count*2U)
    return -1;

  pos=0U;
  for(i=0U;i+1U<count;i++) {
    w1=(unsigned int)get_u16le(data+1U+i*2U);
    cp=w1;

    if(w1>=0xd800U && w1<=0xdbffU && i+2U<count) {
      w2=(unsigned int)get_u16le(data+1U+(i+1U)*2U);
      if(w2>=0xdc00U && w2<=0xdfffU) {
        cp=0x10000U+((w1-0xd800U)<<10)+(w2-0xdc00U);
        i++;
      }
    }

    if(cp=='/' || cp=='\\' || cp<0x20U)
      cp='_';

    if(cp<=0x7fU) {
      if(pos+1U>=out_size)
        return -1;
      out[pos++]=(char)cp;
    } else if(cp<=0x7ffU) {
      if(pos+2U>=out_size)
        return -1;
      out[pos++]=(char)(0xc0U|(cp>>6));
      out[pos++]=(char)(0x80U|(cp&0x3fU));
    } else if(cp<=0xffffU) {
      if(pos+3U>=out_size)
        return -1;
      out[pos++]=(char)(0xe0U|(cp>>12));
      out[pos++]=(char)(0x80U|((cp>>6)&0x3fU));
      out[pos++]=(char)(0x80U|(cp&0x3fU));
    } else if(cp<=0x10ffffU) {
      if(pos+4U>=out_size)
        return -1;
      out[pos++]=(char)(0xf0U|(cp>>18));
      out[pos++]=(char)(0x80U|((cp>>12)&0x3fU));
      out[pos++]=(char)(0x80U|((cp>>6)&0x3fU));
      out[pos++]=(char)(0x80U|(cp&0x3fU));
    } else {
      return -1;
    }
  }

  out[pos]='\0';
  if(strcmp(out,".")==0 || strcmp(out,"..")==0)
    return -1;
  return 0;
}

static int get_object_info(Camera *camera,unsigned int handle,ObjectInfo *info) {
  unsigned int params[1];
  unsigned char *data;
  unsigned int data_len;
  int rc;

  params[0]=handle;
  data=NULL;
  data_len=0U;
  memset(info,0,sizeof(*info));

  if(mtp_exec(camera,PTP_OP_GET_OBJECT_INFO,params,1U,
              &data,&data_len,NULL)<0)
    return -1;
  if(data_len<53U) {
    free(data);
    errno=EPROTO;
    return -1;
  }

  info->format=get_u16le(data+4);
  info->size=get_u32le(data+8);
  rc=mtp_string_to_utf8(data+52,data_len-52U,
                        info->filename,sizeof(info->filename));
  free(data);
  if(rc<0) {
    errno=EPROTO;
    return -1;
  }
  return 0;
}

static char *make_path(const char *dir,const char *filename,const char *suffix) {
  size_t dir_len;
  size_t name_len;
  size_t suffix_len;
  size_t total_len;
  int need_slash;
  char *path;
  size_t pos;

  dir_len=strlen(dir);
  name_len=strlen(filename);
  suffix_len=strlen(suffix);
  need_slash=(dir_len!=0U && dir[dir_len-1]!='/');
  total_len=dir_len+(size_t)need_slash+name_len+suffix_len+1U;

  path=(char *)malloc(total_len);
  if(path==NULL)
    return NULL;

  pos=0U;
  if(dir_len!=0U) {
    memcpy(path+pos,dir,dir_len);
    pos+=dir_len;
  }
  if(need_slash)
    path[pos++]='/';
  if(name_len!=0U) {
    memcpy(path+pos,filename,name_len);
    pos+=name_len;
  }
  if(suffix_len!=0U) {
    memcpy(path+pos,suffix,suffix_len);
    pos+=suffix_len;
  }
  path[pos]='\0';
  return path;
}

static int download_object(Camera *camera,unsigned int handle,
                           const ObjectInfo *info,const char *output_dir) {
  unsigned int params[3];
  unsigned char *data;
  unsigned int data_len;
  unsigned int response_param;
  unsigned int offset;
  unsigned int request_len;
  FILE *file;
  char *final_path;
  char *part_path;
  int rc;

  final_path=make_path(output_dir,info->filename,"");
  part_path=make_path(output_dir,info->filename,".part");
  if(final_path==NULL || part_path==NULL) {
    free(final_path);
    free(part_path);
    errno=ENOMEM;
    return -1;
  }

  file=fopen(part_path,"wb");
  if(file==NULL) {
    free(final_path);
    free(part_path);
    return -1;
  }

  printf("%s (%u bytes)\n",info->filename,info->size);
  fflush(stdout);

  rc=0;
  offset=0U;
  for(;offset<info->size;) {
    request_len=info->size-offset;
    if(request_len>TRANSFER_CHUNK_SIZE)
      request_len=TRANSFER_CHUNK_SIZE;

    params[0]=handle;
    params[1]=offset;
    params[2]=request_len;
    data=NULL;
    data_len=0U;
    response_param=0U;

    if(mtp_exec(camera,PTP_OP_GET_PARTIAL_OBJECT,params,3U,
                &data,&data_len,&response_param)<0) {
      rc=-1;
      break;
    }
    if(data_len==0U || data_len>request_len ||
       data_len>info->size-offset) {
      free(data);
      errno=EPROTO;
      rc=-1;
      break;
    }
    if(fwrite(data,1,(size_t)data_len,file)!=(size_t)data_len) {
      free(data);
      rc=-1;
      break;
    }

    free(data);
    offset+=data_len;
  }

  if(fclose(file)!=0 && rc==0)
    rc=-1;

  if(rc==0 && offset==info->size) {
    if(rename(part_path,final_path)<0)
      rc=-1;
  } else {
    remove(part_path);
  }

  if(rc<0)
    fprintf(stderr,"Failed: %s: %s\n",info->filename,strerror(errno));

  free(final_path);
  free(part_path);
  return rc;
}

static int ensure_output_directory(const char *path) {
  struct stat st;

  if(stat(path,&st)==0) {
    if(!S_ISDIR(st.st_mode)) {
      errno=ENOTDIR;
      return -1;
    }
    return 0;
  }
  if(errno!=ENOENT)
    return -1;
  return mkdir(path,0777);
}

static int process_storage(Camera *camera,unsigned int storage_id,
                           const char *output_dir,unsigned int *files_done,
                           unsigned int *files_failed) {
  unsigned int *handles;
  unsigned int handle_count;
  unsigned int i;
  ObjectInfo info;

  handles=NULL;
  handle_count=0U;
  if(get_object_handles(camera,storage_id,&handles,&handle_count)<0)
    return -1;

  for(i=0U;i<handle_count;i++) {
    if(get_object_info(camera,handles[i],&info)<0) {
      fprintf(stderr,"Cannot read object info for handle 0x%08x: %s\n",
              handles[i],strerror(errno));
      (*files_failed)++;
      continue;
    }

    if(info.format==PTP_FORMAT_ASSOCIATION || info.filename[0]=='\0')
      continue;

    if(download_object(camera,handles[i],&info,output_dir)<0)
      (*files_failed)++;
    else
      (*files_done)++;
  }

  free(handles);
  return 0;
}

int main(int argc,char **argv) {
  Camera camera;
  unsigned int *storage_ids;
  unsigned int storage_count;
  unsigned int files_done;
  unsigned int files_failed;
  unsigned int i;
  long port_value;
  char *end;
  int session_open;
  int rc;

  if(argc!=4) {
    fprintf(stderr,"Usage: %s IP PORT PATH\n",argv[0]);
    return 2;
  }

  errno=0;
  end=NULL;
  port_value=strtol(argv[2],&end,10);
  if(errno!=0 || end==argv[2] || *end!='\0' ||
     port_value<1L || port_value>65535L) {
    fprintf(stderr,"Invalid port: %s\n",argv[2]);
    return 2;
  }

  if(ensure_output_directory(argv[3])<0) {
    fprintf(stderr,"Cannot use output directory %s: %s\n",
            argv[3],strerror(errno));
    return 1;
  }

  signal(SIGPIPE,SIG_IGN);

  camera.command_socket=-1;
  camera.event_socket=-1;
  camera.connection_id=0U;
  camera.transaction_id=0U;
  storage_ids=NULL;
  storage_count=0U;
  files_done=0U;
  files_failed=0U;
  session_open=0;
  rc=1;

  printf("Connecting to %s:%ld\n",argv[1],port_value);
  if(init_command_channel(&camera,argv[1],(unsigned short)port_value)<0) {
    fprintf(stderr,"Command channel failed: %s\n",strerror(errno));
    goto done;
  }

  if(init_event_channel(&camera,argv[1],(unsigned short)port_value)<0) {
    fprintf(stderr,"Event channel failed: %s\n",strerror(errno));
    goto done;
  }

  if(open_session(&camera)<0) {
    fprintf(stderr,"PTP session failed: %s\n",strerror(errno));
    goto done;
  }
  session_open=1;

  if(get_storage_ids(&camera,&storage_ids,&storage_count)<0) {
    fprintf(stderr,"Cannot get storage list: %s\n",strerror(errno));
    goto done;
  }

  for(i=0U;i<storage_count;i++) {
    if(process_storage(&camera,storage_ids[i],argv[3],
                       &files_done,&files_failed)<0) {
      fprintf(stderr,"Storage 0x%08x failed: %s\n",
              storage_ids[i],strerror(errno));
      files_failed++;
    }
  }

  printf("Downloaded: %u  Failed: %u\n",files_done,files_failed);
  rc=(files_failed==0U)?0:1;

done:
  free(storage_ids);
  if(session_open)
    close_session(&camera);
  if(camera.event_socket>=0)
    close(camera.event_socket);
  if(camera.command_socket>=0)
    close(camera.command_socket);
  return rc;
}
