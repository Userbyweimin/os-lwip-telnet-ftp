/**
  ******************************************************************************
  * @file           ftp_server.c
  * @author         ��ô��
  * @brief          ftp ������
  ******************************************************************************
  *
  * COPYRIGHT(c) 2018 GoodMorning
  *
  ******************************************************************************
  */
/* Includes ---------------------------------------------------*/
#include <string.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include "avltree.h"
#include "lwip/opt.h"
#include "lwip/arch.h"
#include "lwip/api.h"
#include "lwip/apps/fs.h" 
#include "ustdio.h"
#include "fatfs.h"



/* Private types ------------------------------------------------------------*/
typedef struct ftp_mbox
{
	struct netconn  * ctrl_port;

	char * arg;
	uint16_t arglen;
	uint16_t event;
	#define FTP_LIST           1U
	#define FTP_SEND_FILE_DATA 2U
	#define FTP_RECV_FILE      3U
	
	char  acCurrentDir[128];
}
ftp_mbox_t;

typedef void (*pfnFTPx_t)(struct ftp_mbox  * pmbox);

typedef struct ftp_cmd
{
	uint32_t	  Index;	 //�����ʶ��
	pfnFTPx_t	  Func;      //��¼�����ָ��
	struct avl_node cmd_node;//avl���ڵ�
}
ftpcmd_t;



/* Private macro ------------------------------------------------------------*/

#define FTP_DATA_PORT 45678U //ftp ���ݶ˿�

//�ַ���ת���Σ��������� FTP �����Ϊһ�� ftp ����ֻ�� 3-4 ���ַ����պÿ���תΪ���ͣ�����Сд
#define FTP_STR2ID(str) ((*(int*)(str)) & 0xDFDFDFDF) 

// ftp ����������
#define FTP_REGISTER_COMMAND(CMD) \
	do{\
		static struct ftp_cmd CmdBuf ;      \
		CmdBuf.Index = FTP_STR2ID(#CMD);    \
		CmdBuf.Func  = ctrl_port_reply_##CMD;\
		ftp_insert_command(&CmdBuf);            \
	}while(0)


// ftp �ļ��б��ʽ
#define NORMAL_LIST(listbuf,filesize,month,day,year,filename)\
	sprintf(listbuf,normal_format,(filesize),month_list[(month)],(day),(year),(filename))

#define THIS_YEAR_LIST(listbuf,filesize,month,day,hour,min,filename)\
	sprintf(listbuf,this_year_format,(filesize),month_list[(month)],(day),(hour),(min),(filename))


// ftp ��ʽһ��Ϊ xxxx /dirx/diry/\r\n ,ȥ�� /\r\n ��ȡ����·�� 	
#define vFtp_GetLegalPath(path,pathend) 	\
	do{\
		while(*path == ' ')  ++path;         \
		if (*pathend == '\n') *pathend-- = 0;\
		if (*pathend == '\r') *pathend-- = 0;\
		if (*pathend == '/' ) *pathend = 0;\
	}while(0)
			
#define LEGAL_PATH(path) 	\
	do{\
		char * pathend = path;\
		while(*path == ' ')  ++path;        \
		while(*pathend) ++pathend;		    \
		if (*(--pathend) == '\n') *pathend-- = 0;\
		if (*pathend == '\r') *pathend-- = 0;\
		if (*pathend == '/' ) *pathend = 0;  \
	}while(0)

/* Private variables ------------------------------------------------------------*/

static const char normal_format[]   = "-rw-rw-rw-   1 user     ftp  %11ld %s %02i %5i %s\r\n";
static const char this_year_format[] = "-rw-rw-rw-   1 user     ftp  %11ld %s %02i %02i:%02i %s\r\n";
static const char  * month_list[] = { //�·ݴ� 1 �� 12 ��0 ��� NULL 
	NULL,
	"Jan","Feb","Mar","Apr","May","Jun",
	"Jul","Aug","Sep","Oct","Nov","Dez" }; 


static struct avl_root ftp_root = {.avl_node = NULL};//����ƥ���ƽ����������� 


static const char ftp_msg_451[] = "451 errors";
static const char ftp_msg_226[] = "226 transfer complete\r\n";



osMessageQId  osFtpDataPortmbox;// ���ݶ˿ڴ�������

extern uint8_t IP_ADDRESS[4];//from lwip.c


/* Private function prototypes -----------------------------------------------*/
static void vFtpDataPortPro(void const * arg);





/* Gorgeous Split-line -----------------------------------------------*/

/**
	* @brief    ftp_insert_command 
	*           ����������
	* @param    pCmd        ������ƿ�
	* @return   �ɹ����� 0
*/
static int ftp_insert_command(struct ftp_cmd * pCmd)
{
	struct avl_node **tmp = &ftp_root.avl_node;
 	struct avl_node *parent = NULL;
	
	/* Figure out where to put new node */
	while (*tmp)
	{
		struct ftp_cmd *this = container_of(*tmp, struct ftp_cmd, cmd_node);

		parent = *tmp;
		if (pCmd->Index < this->Index)
			tmp = &((*tmp)->avl_left);
		else 
		if (pCmd->Index > this->Index)
			tmp = &((*tmp)->avl_right);
		else
			return 1;
	}

	/* Add new node and rebalance tree. */
	//rb_link_node(&pCmd->cmd_node, parent, tmp);
	//rb_insert_color(&pCmd->cmd_node, root);
	avl_insert(&ftp_root,&pCmd->cmd_node,parent,tmp);
	
	return 0;
}


/**
	* @brief    ftp_search_command 
	*           ���������ң����� Index ���ҵ���Ӧ�Ŀ��ƿ�
	* @param    Index        �����
	* @return   �ɹ� Index �Ŷ�Ӧ�Ŀ��ƿ�
*/
static struct ftp_cmd *ftp_search_command(int iCtrlCmd)
{
    struct avl_node *node = ftp_root.avl_node;

    while (node) 
	{
		struct ftp_cmd *pCmd = container_of(node, struct ftp_cmd, cmd_node);

		if (iCtrlCmd < pCmd->Index)
		    node = node->avl_left;
		else 
		if (iCtrlCmd > pCmd->Index)
		    node = node->avl_right;
  		else 
			return pCmd;
    }
    
    return NULL;
}





/**
	* @brief    ctrl_port_reply_USER 
	*           ftp ����˿����� USER ��ϵͳ��½���û���
	* @param    arg ������������
	* @return   NULL
*/
static void ctrl_port_reply_USER(struct ftp_mbox * msgbox)
{
	static const char reply_msg[] = "230 Operation successful\r\n";  //230 ��½������
	netconn_write(msgbox->ctrl_port,reply_msg,sizeof(reply_msg)-1,NETCONN_NOCOPY);
}


/**
	* @brief    ctrl_port_reply_SYST 
	*           ftp ����˿����� SYST �����ط�����ʹ�õĲ���ϵͳ
	* @param    arg ������������
	* @return   NULL
*/
static void ctrl_port_reply_SYST(struct ftp_mbox * msgbox)
{
	static const char reply_msg[] = "215 UNIX Type: L8\r\n";  //215 ϵͳ���ͻظ�
	netconn_write(msgbox->ctrl_port,reply_msg,sizeof(reply_msg)-1,NETCONN_NOCOPY);
}


/**
	* @brief    ctrl_port_reply_PWD 
	*           ftp ����˿����� PWD
	* @param    arg ������������
	* @return   NULL
*/
static void ctrl_port_reply_PWD(struct ftp_mbox * msgbox) //��ʾ��ǰ����Ŀ¼
{
	#if 1
	char reply_msg[128] ;//257 ·��������
	sprintf(reply_msg,"257 \"%s/\"\r\n",msgbox->acCurrentDir);
	#else
	static const char reply_msg[] = "257 \"/\"\r\n";
	#endif
	netconn_write(msgbox->ctrl_port,reply_msg,strlen(reply_msg),NETCONN_COPY);
}


/**
	* @brief    ctrl_port_reply_NOOP 
	*           ftp ����˿����� NOOP
	* @param    arg ������������
	* @return   NULL
*/
static void ctrl_port_reply_NOOP(struct ftp_mbox * msgbox)
{
	static const char reply_msg[] = "200 Operation successful\r\n";
	netconn_write(msgbox->ctrl_port,reply_msg,sizeof(reply_msg)-1,NETCONN_NOCOPY);
}


/**
	* @brief    ctrl_port_reply_CWD 
	*           ftp ����˿����� CWD
	* @param    arg ������������
	* @return   NULL
*/
static void ctrl_port_reply_CWD(struct ftp_mbox * msgbox)
{
	static const char reply_msg[] = "250 Operation successful\r\n"; //257 ·��������

	DIR fsdir;
	char * pcFilePath = msgbox->arg;
	char * pcPathEnd = msgbox->arg + msgbox->arglen - 1;
	
	vFtp_GetLegalPath(pcFilePath,pcPathEnd);
	
	printk("cwd %s\r\n",pcFilePath);
	
	if (FR_OK != f_opendir(&fsdir,pcFilePath))
	{
		printk("illegal path\r\n");
		goto CWDdone ;
	}

	f_closedir(&fsdir);
	
	if (pcPathEnd != pcFilePath)
		memcpy(msgbox->acCurrentDir,pcFilePath,pcPathEnd - pcFilePath);
	
	msgbox->acCurrentDir[pcPathEnd - pcFilePath] = 0;

CWDdone:

	netconn_write(msgbox->ctrl_port,reply_msg,sizeof(reply_msg)-1,NETCONN_NOCOPY);
}


/**
	* @brief    ctrl_port_reply_PASV 
	*           ftp ����˿����� PASV ������ģʽ
	*           
	* @param    arg ������������
	* @return   NULL
*/
static void ctrl_port_reply_PASV(struct ftp_mbox * msgbox)
{
	static char reply_msg[64] = {0} ; //"227 PASV ok(192,168,40,104,185,198)\r\n"

	uint32_t iFtpRplyLen = strlen(reply_msg);
	
	if (0 == iFtpRplyLen) // δ��ʼ����Ϣ
	{
		sprintf(reply_msg,"227 PASV ok(%d,%d,%d,%d,%d,%d)\r\n",
			IP_ADDRESS[0],IP_ADDRESS[1],IP_ADDRESS[2],IP_ADDRESS[3],(FTP_DATA_PORT>>8),FTP_DATA_PORT&0x00ff);

		iFtpRplyLen = strlen(reply_msg);
	}
	
	netconn_write(msgbox->ctrl_port,reply_msg,iFtpRplyLen,NETCONN_NOCOPY);
	
	printk("data port standby\r\n");
}





/**
	* @brief    ctrl_port_reply_LIST 
	*           ftp ����˿����� LIST , ��ȡ��ǰ�ļ��б�
	* @param    arg ������������
	* @return   NULL
*/
static void ctrl_port_reply_LIST(struct ftp_mbox * msgbox)
{
	static const char reply_msg[] = "150 Directory listing\r\n" ;//150 ������
	//1.�ڿ��ƶ˿ڶ� LIST ������лظ�
	//2.�����ݶ˿ڷ��� "total 0"�����ò�ƿ���û��
	//3.�����ݶ˿ڷ����ļ��б�
	//4.�ر����ݶ˿�
	
	netconn_write(msgbox->ctrl_port,reply_msg,sizeof(reply_msg)-1,NETCONN_NOCOPY);
	msgbox->event = FTP_LIST; //�¼�Ϊ�б��¼�

	//���ʹ���Ϣ�����ݶ˿�����
	while(osMessagePut(osFtpDataPortmbox,(uint32_t)msgbox , osWaitForever) != osOK);

}


/**
	* @brief    ctrl_port_reply_SIZE
	*           ftp ����˿����� SIZE , ��ȡ��ǰ�ļ��б�
	* @param    arg ������������
	* @return   NULL
*/
static void ctrl_port_reply_SIZE(struct ftp_mbox * msgbox)
{
	char acFtpBuf[128];
	uint32_t iFileSize;
	char * pcFilePath = msgbox->arg;
	char * pcPathEnd  = msgbox->arg + msgbox->arglen - 1;

	vFtp_GetLegalPath(pcFilePath,pcPathEnd);

	if (*pcFilePath != '/')//���·����ȫΪ����·��
	{
		sprintf(acFtpBuf,"%s/%s",msgbox->acCurrentDir,pcFilePath);
		pcFilePath = acFtpBuf;
	}

	if (FR_OK != f_open(&SDFile,pcFilePath,FA_READ))
	{
		sprintf(acFtpBuf,"213 0\r\n");
		goto SIZEdone;
	}

	iFileSize = f_size(&SDFile);
	sprintf(acFtpBuf,"213 %d\r\n",iFileSize);
	f_close(&SDFile);

SIZEdone:	
	netconn_write(msgbox->ctrl_port,acFtpBuf,strlen(acFtpBuf),NETCONN_COPY);
}



/**
	* @brief    ctrl_port_reply_RETR
	*           ftp ����˿����� RETR
	* @param    arg ������������
	* @return   NULL
*/
static void ctrl_port_reply_RETR(struct ftp_mbox * msgbox)
{
	static const char reply_msg[] = "108 Operation successful\r\n" ;

	netconn_write(msgbox->ctrl_port,reply_msg,sizeof(reply_msg)-1,NETCONN_COPY);
	
	msgbox->event = FTP_SEND_FILE_DATA; 

	//���ʹ���Ϣ�����ݶ˿�����
	while(osMessagePut(osFtpDataPortmbox,(uint32_t)msgbox , osWaitForever) != osOK);
}



/**
	* @brief    ctrl_port_reply_DELE
	*           ftp ����˿����� RETR
	* @param    arg ������������
	* @return   NULL
*/
static void ctrl_port_reply_DELE(struct ftp_mbox * msgbox)
{
	static const char reply_msgOK[] = "250 Operation successful\r\n" ;
	static const char reply_msgError[] = "450 Operation error\r\n" ;
	FRESULT res;
	char databuf[128];
	char * pcFilePath = msgbox->arg;
	char * pcPathEnd = msgbox->arg + msgbox->arglen - 1;
	vFtp_GetLegalPath(pcFilePath,pcPathEnd);

//	LEGAL_PATH(pcFilePath);

	if (*pcFilePath != '/')//���·��
	{
		sprintf(databuf,"%s/%s",msgbox->acCurrentDir,pcFilePath);
		pcFilePath = databuf;
	}
	
	printk("dele:%s\r\n",pcFilePath);

	res = f_unlink(pcFilePath);
	if (FR_OK != res)
		goto DeleError;
	
	netconn_write(msgbox->ctrl_port,reply_msgOK,sizeof(reply_msgOK)-1,NETCONN_NOCOPY);
	return ;

DeleError:	
	netconn_write(msgbox->ctrl_port,reply_msgError,sizeof(reply_msgError)-1,NETCONN_NOCOPY);

	printk("dele error code:%d\r\n",res);
	return ;
	
}


/**
	* @brief    ctrl_port_reply_STOR
	*           ftp ����˿����� STOR
	* @param    arg ������������
	* @return   NULL
*/
static void ctrl_port_reply_STOR(struct ftp_mbox * msgbox)
{
	static const char reply_msgOK[] = "125 Waiting\r\n" ;

	netconn_write(msgbox->ctrl_port,reply_msgOK,sizeof(reply_msgOK)-1,NETCONN_NOCOPY);

	msgbox->event = FTP_RECV_FILE;

	//���ʹ���Ϣ�����ݶ˿�����
	while(osMessagePut(osFtpDataPortmbox,(uint32_t)msgbox , osWaitForever) != osOK);
}


/**
	* @brief    vFtpCtrlPortPro 
	*           tcp ��������������
	* @param    arg �������
	* @return   void
*/
static void vFtpCtrlPortPro(void const * arg)
{
	static const char ftp_reply_unkown[] = "500 Unknown command\r\n";
	static const char ftp_connect_msg[] = "220 Operation successful\r\n";

	char *    pcRxbuf;
	uint16_t  sRxlen;
 	uint32_t iCtrlCmd ;
	
	struct netbuf   * pFtpCmdBuf;
	
	struct ftp_cmd * pCmdMatch;
	struct ftp_mbox  msgbox;

	msgbox.ctrl_port = (struct netconn *)arg; //��ǰ ftp ���ƶ˿����Ӿ��	
	msgbox.acCurrentDir[0] = 0;               //·��Ϊ�գ�����Ŀ¼
	
	netconn_write(msgbox.ctrl_port,ftp_connect_msg,sizeof(ftp_connect_msg)-1,NETCONN_NOCOPY);

	while(ERR_OK == netconn_recv(msgbox.ctrl_port, &pFtpCmdBuf))  //����ֱ���յ�����
	{
		do
		{
			netbuf_data(pFtpCmdBuf, (void**)&pcRxbuf, &sRxlen); //��ȡ����ָ��

			iCtrlCmd = FTP_STR2ID(pcRxbuf);
			if ( pcRxbuf[3] < 'A' || pcRxbuf[3] > 'z' )//��Щ����ֻ�������ֽڣ���Ҫ�ж�
			{
				iCtrlCmd &= 0x00ffffff;
				msgbox.arg = pcRxbuf + 4;
				msgbox.arglen = sRxlen - 4;
			}
			else
			{
				msgbox.arg = pcRxbuf + 5;
				msgbox.arglen = sRxlen - 5;
			}
			
			pCmdMatch = ftp_search_command(iCtrlCmd);//ƥ�������
			
			if (NULL == pCmdMatch)
				netconn_write(msgbox.ctrl_port,ftp_reply_unkown,sizeof(ftp_reply_unkown)-1,NETCONN_NOCOPY);
			else
				pCmdMatch->Func(&msgbox);
		}
		while(netbuf_next(pFtpCmdBuf) >= 0);
		
		netbuf_delete(pFtpCmdBuf);
	}

	netconn_close(msgbox.ctrl_port); //�ر�����
	netconn_delete(msgbox.ctrl_port);//����ͷ����ӵ��ڴ�
	
	color_printk(green,"\r\n|!ftp disconnect!|\r\n");
	
	vTaskDelete(NULL);//���ӶϿ�ʱɾ���Լ�
}






/* Start node to be scanned (***also used as work area***) */
static char * data_port_list_file (struct netconn * data_port_conn,struct ftp_mbox * msgbox)
{
	char * ctrl_msg = (char *)ftp_msg_226;
	char   list_buf[128] ;
    DIR dir;
	FILINFO fno;

	if (FR_OK != f_opendir(&dir, msgbox->acCurrentDir))
	{
		goto ScanDirDone ;
	}

    for (;;) 
	{
		struct FileDate * pStDate ;
		struct FileTime * pStTime ;
        FRESULT res = f_readdir(&dir, &fno); /* Read a directory item */
		
		if (res != FR_OK || fno.fname[0] == 0) /* Break on error or end of dir */
			break; 

		if ( (fno.fattrib & AM_DIR) && (fno.fattrib != AM_DIR))//����ʾֻ��/ϵͳ/�����ļ���
			continue;

		pStDate = (struct FileDate *)(&fno.fdate);
		pStTime = (struct FileTime *)(&fno.ftime);
		
		if (fno.fdate == 0 || fno.ftime == 0) //û�����ڵ��ļ�
			NORMAL_LIST(list_buf,fno.fsize,1,1,1980,fno.fname);
		else
		if (pStDate->Year + 1980 == 2018) //ͬһ����ļ�
			THIS_YEAR_LIST(list_buf,fno.fsize,pStDate->Month,pStDate->Day,pStTime->Hour,pStTime->Min,fno.fname);
		else
			NORMAL_LIST(list_buf,fno.fsize,pStDate->Month,pStDate->Day,pStDate->Year+1980,fno.fname);
		
		if (fno.fattrib & AM_DIR )   /* It is a directory */
			list_buf[0] = 'd';
		
		netconn_write(data_port_conn,list_buf,strlen(list_buf),NETCONN_COPY);
    }
	
	f_closedir(&dir); //��·���ر�

ScanDirDone:
	return ctrl_msg;
}




static char * data_port_send_file(struct netconn * data_port_conn,struct ftp_mbox * msgbox)
{
	char * ctrl_msg = (char *)ftp_msg_451;
	
	FIL FileSend; 		 /* File object */
	FRESULT res ;
	uint32_t iFileSize;
	uint32_t iReadSize;
	char * pcFilePath = msgbox->arg;
	char * pcPathEnd = msgbox->arg + msgbox->arglen - 1;
	char  databuf[128];

	vFtp_GetLegalPath(pcFilePath,pcPathEnd);

	if (*pcFilePath != '/')//���·��
	{
		sprintf(databuf,"%s/%s",msgbox->acCurrentDir,pcFilePath);
		pcFilePath = databuf;
	}

	res = f_open(&FileSend,pcFilePath,FA_READ);
	if (FR_OK != res)
	{
		Errors("cannot open \"%s\",code = %d",pcFilePath,res);
		goto SendEnd;
	}

	iFileSize = f_size(&FileSend);

	while(iFileSize)
	{
		res = f_read(&FileSend,databuf,sizeof(databuf),&iReadSize);//С������
		if ((FR_OK != res) || (0 == iReadSize))
		{
			Errors("Cannot read \"%s\",error code :%d\r\n",pcFilePath,res);
			goto SendEnd;
		}
		else
		{
			netconn_write(data_port_conn,databuf,iReadSize,NETCONN_COPY);
			iFileSize -= iReadSize;
		}
	}
	
	ctrl_msg = (char *)ftp_msg_226;
	f_close(&FileSend);

SendEnd:

	return ctrl_msg;//216
}



static char * data_port_recv_file(struct netconn * data_port_conn,struct ftp_mbox * msgbox)
{
	static __align(4) char recv_buf[TCP_MSS] ;// fatfs д�ļ���ʱ��bufҪ��ַ���룬�������׳���
	
	char * ctrl_msg = (char *)ftp_msg_451;
	FIL RecvFile; 		 /* File object */
	FRESULT res;
	char * pcFile = msgbox->arg;
	char * pcPathEnd = msgbox->arg + msgbox->arglen - 1;
	char   databuf[128];
	uint16_t sRxlen;
	uint32_t byteswritten;
	struct netbuf  * data_netbuf;

	vFtp_GetLegalPath(pcFile,pcPathEnd);

	if (*pcFile != '/')//���·��
	{
		sprintf(databuf,"%s/%s",msgbox->acCurrentDir,pcFile);
		pcFile = databuf;
	}
	
	res = f_open(&RecvFile, pcFile, FA_CREATE_ALWAYS | FA_WRITE);
	if(res != FR_OK) 
	{
		Errors("cannot open/create \"%s\",error code = %d\r\n",pcFile,res);
		goto RecvEnd;
	}
	printk("recvfile");
	while(ERR_OK == netconn_recv(data_port_conn, &data_netbuf))  //����ֱ���յ�����
	{
		do{
			netbuf_data(data_netbuf, (void**)&pcFile, &sRxlen); //��ȡ����ָ��

			#if 1
			memcpy(recv_buf,pcFile,sRxlen);//�����ݿ��������������׳���
			pcFile = recv_buf;
			#endif
			
			res = f_write(&RecvFile,(void*)pcFile, sRxlen, &byteswritten);
	
			printk(".");
			if ((byteswritten == 0) || (res != FR_OK))
			{
				f_close(&RecvFile);
				Errors("write file error\r\n");
				goto RecvEnd;
			}
		}
		while(netbuf_next(data_netbuf) >= 0);
		
		netbuf_delete(data_netbuf);
	}
	
	printk("done\r\n");

	ctrl_msg = (char *)ftp_msg_226;
	f_close(&RecvFile);

RecvEnd:
	
	return ctrl_msg;
}



/**
	* @brief    vFtpDataPortPro 
	*           tcp ��������������
	* @param    arg �������
	* @return   void
*/
static void vFtpDataPortPro(void const * arg)
{	
	struct netconn * data_port_listen;
	struct netconn * data_port_conn;
	
	struct ftp_mbox * msgbox;
	char * ctrl_msg;
	osEvent event;

	data_port_listen = netconn_new(NETCONN_TCP); //����һ�� TCP ����
	netconn_bind(data_port_listen,IP_ADDR_ANY,FTP_DATA_PORT); //�� ���ݶ˿�
	netconn_listen(data_port_listen); //�������ģʽ

	while(1)
	{
		if (ERR_OK == netconn_accept(data_port_listen,&data_port_conn)) //����ֱ���� ftp ��������
		{
			event = osMessageGet(osFtpDataPortmbox,osWaitForever);//�ȴ���������
			msgbox = (struct ftp_mbox *)(event.value.p);//��ȡ��������

			switch(msgbox->event) //���ݲ�ͬ�Ĳ���������в���
			{
				case FTP_LIST :
					ctrl_msg = data_port_list_file(data_port_conn,msgbox);
					break;
				
				case FTP_SEND_FILE_DATA:
					ctrl_msg = data_port_send_file(data_port_conn,msgbox);
					break;
					
				case FTP_RECV_FILE:
					ctrl_msg = data_port_recv_file(data_port_conn,msgbox);
					break;

				default: ;
			}

			netconn_write(msgbox->ctrl_port,ctrl_msg,strlen(ctrl_msg),NETCONN_NOCOPY);//���ƶ˿ڷ���
			
			netconn_close(data_port_conn); //�ر����ݶ˿�����
			netconn_delete(data_port_conn);//����ͷ����ӵ��ڴ�
			
			printk("data port shutdown!\r\n");
		}
		else
		{
			Warnings("data port accept error\r\n");
		}
	}
		
	netconn_close(data_port_listen); //�ر�����
	netconn_delete(data_port_listen);//����ͷ����ӵ��ڴ�

	vTaskDelete(NULL);//���ӶϿ�ʱɾ���Լ�

}



/**
	* @brief    vFtp_ServerConn 
	*           tcp ��������������
	* @param    arg �������
	* @return   void
*/
static void vFtp_ServerConn(void const * arg)
{
	struct netconn * pFtpCtrlPortListen;
	struct netconn * pFtpCtrlPortConn;
	err_t err;

	pFtpCtrlPortListen = netconn_new(NETCONN_TCP); //����һ�� TCP ����
	netconn_bind(pFtpCtrlPortListen,IP_ADDR_ANY,21); //�󶨶˿� 21 �Ŷ˿�
	netconn_listen(pFtpCtrlPortListen); //�������ģʽ

	for(;;)
	{
		err = netconn_accept(pFtpCtrlPortListen,&pFtpCtrlPortConn); //����ֱ���� ftp ��������
		
		if (err == ERR_OK) //�����ӳɹ�ʱ������һ�����̴߳��� ftp ����
		{
		  osThreadDef(FtpCtrl, vFtpCtrlPortPro, osPriorityNormal, 0, 500);
		  osThreadCreate(osThread(FtpCtrl), pFtpCtrlPortConn);
		}
		else
		{
			Warnings("%s():ftp not accept\r\n",__FUNCTION__);
		}
	}
}


void vFtp_ServerInit(void)
{
	pfnFTPx_t ctrl_port_reply_TYPE = ctrl_port_reply_NOOP;

	//������ص����������
	FTP_REGISTER_COMMAND(USER);
	FTP_REGISTER_COMMAND(SYST);
	FTP_REGISTER_COMMAND(PWD);
	FTP_REGISTER_COMMAND(CWD);
	FTP_REGISTER_COMMAND(PASV);
	FTP_REGISTER_COMMAND(LIST);
	FTP_REGISTER_COMMAND(NOOP);
	FTP_REGISTER_COMMAND(TYPE);
	FTP_REGISTER_COMMAND(SIZE);
	FTP_REGISTER_COMMAND(RETR);
	FTP_REGISTER_COMMAND(DELE);
	FTP_REGISTER_COMMAND(STOR);
	
	osThreadDef(FtpServer, vFtp_ServerConn, osPriorityNormal, 0, 128);
	osThreadCreate(osThread(FtpServer), NULL);//��ʼ���� lwip �󴴽�tcp��������������

	osThreadDef(FtpData, vFtpDataPortPro, osPriorityNormal, 0, 512);
	osThreadCreate(osThread(FtpData), NULL);//��ʼ���� lwip �󴴽�tcp��������������

	osMessageQDef(osFtpMBox, 4,void *);
	osFtpDataPortmbox = osMessageCreate(osMessageQ(osFtpMBox),NULL);
}

