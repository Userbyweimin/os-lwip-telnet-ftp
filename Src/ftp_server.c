

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
	struct netconn  * pCtrlConn;

	char * pcArg;
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
#define vFTP_RegisterCommand(CMD) \
	do{\
		static struct ftp_cmd CmdBuf ;      \
		CmdBuf.Index = FTP_STR2ID(#CMD);    \
		CmdBuf.Func  = vFtpCtrl_Cmd_##CMD;\
		iFtp_InsertCmd(&CmdBuf);            \
	}while(0)


// ftp �ļ��б��ʽ
#define vFtp_NormalList(listbuf,filesize,month,day,year,filename)\
	sprintf(listbuf,acNormalListFormat,(filesize),acMonthList[(month)],(day),(year),(filename))

#define vFtp_ThisYearList(listbuf,filesize,month,day,hour,min,filename)\
	sprintf(listbuf,acThisYearListFormat,(filesize),acMonthList[(month)],(day),(hour),(min),(filename))


// ftp ��ʽһ��Ϊ xxxx /dirx/diry/\r\n ,ȥ�� /\r\n ��ȡ����·�� 	
#define vFtp_GetLegalPath(path,pathend) 	\
	do{\
		while(*path == ' ')  ++path;         \
		if (*pathend == '\n') *pathend-- = 0;\
		if (*pathend == '\r') *pathend-- = 0;\
		if (*pathend == '/' ) *pathend = 0;\
	}while(0)
			
#define vFtp_LegalPath(path) 	\
	do{\
		char * pathend = path;\
		while(*path == ' ')  ++path;        \
		while(*pathend) ++pathend;		    \
		if (*(--pathend) == '\n') *pathend-- = 0;\
		if (*pathend == '\r') *pathend-- = 0;\
		if (*pathend == '/' ) *pathend = 0;  \
	}while(0)

/* Private variables ------------------------------------------------------------*/

static const char acNormalListFormat[]   = "-rw-rw-rw-   1 user     ftp  %11ld %s %02i %5i %s\r\n";
static const char acThisYearListFormat[] = "-rw-rw-rw-   1 user     ftp  %11ld %s %02i %02i:%02i %s\r\n";

static const char  * acMonthList[] = { //�·ݴ� 1 �� 12 ��0 ��� NULL 
	NULL,"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dez" };   

static struct avl_root stFtpCmdTreeRoot = {.avl_node = NULL};//����ƥ���ƽ����������� 


static const char acFtpCtrlMsg451[] = "451 errors";
static const char acFtpCtrlMsg226[] = "226 transfer complete\r\n";



osMessageQId  osFtpDataPortmbox;// ���ݶ˿ڴ�������

extern uint8_t IP_ADDRESS[4];//from lwip.c


/* Private function prototypes -----------------------------------------------*/
static void vFtpDataPortPro(void const * arg);





/* Gorgeous Split-line -----------------------------------------------*/

/**
	* @brief    iFtp_InsertCmd 
	*           ����������
	* @param    pCmd        ������ƿ�
	* @return   �ɹ����� 0
*/
static int iFtp_InsertCmd(struct ftp_cmd * pCmd)
{
	struct avl_node **tmp = &stFtpCmdTreeRoot.avl_node;
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
	avl_insert(&stFtpCmdTreeRoot,&pCmd->cmd_node,parent,tmp);
	
	return 0;
}


/**
	* @brief    pFtp_SearchCmd 
	*           ���������ң����� Index ���ҵ���Ӧ�Ŀ��ƿ�
	* @param    Index        �����
	* @return   �ɹ� Index �Ŷ�Ӧ�Ŀ��ƿ�
*/
static struct ftp_cmd *pFtp_SearchCmd(int iCtrlCmd)
{
    struct avl_node *node = stFtpCmdTreeRoot.avl_node;

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
	* @brief    vFtpCtrl_Cmd_USER 
	*           ftp ����˿����� USER ��ϵͳ��½���û���
	* @param    arg ������������
	* @return   NULL
*/
static void vFtpCtrl_Cmd_USER(struct ftp_mbox * pFtpMBox)
{
	static const char acFtpReply[] = "230 Operation successful\r\n";  //230 ��½������
	netconn_write(pFtpMBox->pCtrlConn,acFtpReply,sizeof(acFtpReply)-1,NETCONN_NOCOPY);
}


/**
	* @brief    vFtpCtrl_Cmd_SYST 
	*           ftp ����˿����� SYST �����ط�����ʹ�õĲ���ϵͳ
	* @param    arg ������������
	* @return   NULL
*/
static void vFtpCtrl_Cmd_SYST(struct ftp_mbox * pFtpMBox)
{
	static const char acFtpReply[] = "215 UNIX Type: L8\r\n";  //215 ϵͳ���ͻظ�
	netconn_write(pFtpMBox->pCtrlConn,acFtpReply,sizeof(acFtpReply)-1,NETCONN_NOCOPY);
}


/**
	* @brief    vFtpCtrl_Cmd_PWD 
	*           ftp ����˿����� PWD
	* @param    arg ������������
	* @return   NULL
*/
static void vFtpCtrl_Cmd_PWD(struct ftp_mbox * pFtpMBox) //��ʾ��ǰ����Ŀ¼
{
	#if 1
	char acFtpReply[128] ;//257 ·��������
	sprintf(acFtpReply,"257 \"%s/\"\r\n",pFtpMBox->acCurrentDir);
	#else
	static const char acFtpReply[] = "257 \"/\"\r\n";
	#endif
	netconn_write(pFtpMBox->pCtrlConn,acFtpReply,strlen(acFtpReply),NETCONN_COPY);
}


/**
	* @brief    vFtpCtrl_Cmd_NOOP 
	*           ftp ����˿����� NOOP
	* @param    arg ������������
	* @return   NULL
*/
static void vFtpCtrl_Cmd_NOOP(struct ftp_mbox * pFtpMBox)
{
	static const char acFtpReply[] = "200 Operation successful\r\n";
	netconn_write(pFtpMBox->pCtrlConn,acFtpReply,sizeof(acFtpReply)-1,NETCONN_NOCOPY);
}


/**
	* @brief    vFtpCtrl_Cmd_CWD 
	*           ftp ����˿����� CWD
	* @param    arg ������������
	* @return   NULL
*/
static void vFtpCtrl_Cmd_CWD(struct ftp_mbox * pFtpMBox)
{
	static const char acFtpReply[] = "250 Operation successful\r\n"; //257 ·��������

	DIR fsdir;
	char * pcFilePath = pFtpMBox->pcArg;
	char * pcPathEnd = pFtpMBox->pcArg + pFtpMBox->arglen - 1;
	
	vFtp_GetLegalPath(pcFilePath,pcPathEnd);
	
	printk("cwd %s\r\n",pcFilePath);
	
	if (FR_OK != f_opendir(&fsdir,pcFilePath))
	{
		printk("illegal path\r\n");
		goto CWDdone ;
	}

	f_closedir(&fsdir);
	
	if (pcPathEnd != pcFilePath)
		memcpy(pFtpMBox->acCurrentDir,pcFilePath,pcPathEnd - pcFilePath);
	
	pFtpMBox->acCurrentDir[pcPathEnd - pcFilePath] = 0;

CWDdone:

	netconn_write(pFtpMBox->pCtrlConn,acFtpReply,sizeof(acFtpReply)-1,NETCONN_NOCOPY);
}


/**
	* @brief    vFtpCtrl_Cmd_PASV 
	*           ftp ����˿����� PASV ������ģʽ
	*           
	* @param    arg ������������
	* @return   NULL
*/
static void vFtpCtrl_Cmd_PASV(struct ftp_mbox * pFtpMBox)
{
	static char acFtpReply[64] = {0} ; //"227 PASV ok(192,168,40,104,185,198)\r\n"

	uint32_t iFtpRplyLen = strlen(acFtpReply);
	
	if (0 == iFtpRplyLen) // δ��ʼ����Ϣ
	{
		sprintf(acFtpReply,"227 PASV ok(%d,%d,%d,%d,%d,%d)\r\n",
			IP_ADDRESS[0],IP_ADDRESS[1],IP_ADDRESS[2],IP_ADDRESS[3],(FTP_DATA_PORT>>8),FTP_DATA_PORT&0x00ff);

		iFtpRplyLen = strlen(acFtpReply);
	}
	
	netconn_write(pFtpMBox->pCtrlConn,acFtpReply,iFtpRplyLen,NETCONN_NOCOPY);
	
	printk("data port standby\r\n");
}





/**
	* @brief    vFtpCtrl_Cmd_LIST 
	*           ftp ����˿����� LIST , ��ȡ��ǰ�ļ��б�
	* @param    arg ������������
	* @return   NULL
*/
static void vFtpCtrl_Cmd_LIST(struct ftp_mbox * pFtpMBox)
{
	static const char acFtpReply[] = "150 Directory listing\r\n" ;//150 ������
	//1.�ڿ��ƶ˿ڶ� LIST ������лظ�
	//2.�����ݶ˿ڷ��� "total 0"�����ò�ƿ���û��
	//3.�����ݶ˿ڷ����ļ��б�
	//4.�ر����ݶ˿�
	
	netconn_write(pFtpMBox->pCtrlConn,acFtpReply,sizeof(acFtpReply)-1,NETCONN_NOCOPY);
	pFtpMBox->event = FTP_LIST; //�¼�Ϊ�б��¼�

	//���ʹ���Ϣ�����ݶ˿�����
	while(osMessagePut(osFtpDataPortmbox,(uint32_t)pFtpMBox , osWaitForever) != osOK);

}


/**
	* @brief    vFtpCtrl_Cmd_SIZE
	*           ftp ����˿����� SIZE , ��ȡ��ǰ�ļ��б�
	* @param    arg ������������
	* @return   NULL
*/
static void vFtpCtrl_Cmd_SIZE(struct ftp_mbox * pFtpMBox)
{
	char acFtpBuf[128];
	uint32_t iFileSize;
	char * pcFilePath = pFtpMBox->pcArg;
	char * pcPathEnd  = pFtpMBox->pcArg + pFtpMBox->arglen - 1;

	vFtp_GetLegalPath(pcFilePath,pcPathEnd);

	if (*pcFilePath != '/')//���·����ȫΪ����·��
	{
		sprintf(acFtpBuf,"%s/%s",pFtpMBox->acCurrentDir,pcFilePath);
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
	netconn_write(pFtpMBox->pCtrlConn,acFtpBuf,strlen(acFtpBuf),NETCONN_COPY);
}



/**
	* @brief    vFtpCtrl_Cmd_RETR
	*           ftp ����˿����� RETR
	* @param    arg ������������
	* @return   NULL
*/
static void vFtpCtrl_Cmd_RETR(struct ftp_mbox * pFtpMBox)
{
	static const char acFtpReply[] = "108 Operation successful\r\n" ;

	netconn_write(pFtpMBox->pCtrlConn,acFtpReply,sizeof(acFtpReply)-1,NETCONN_COPY);
	
	pFtpMBox->event = FTP_SEND_FILE_DATA; 

	//���ʹ���Ϣ�����ݶ˿�����
	while(osMessagePut(osFtpDataPortmbox,(uint32_t)pFtpMBox , osWaitForever) != osOK);
}



/**
	* @brief    vFtpCtrl_Cmd_DELE
	*           ftp ����˿����� RETR
	* @param    arg ������������
	* @return   NULL
*/
static void vFtpCtrl_Cmd_DELE(struct ftp_mbox * pFtpMBox)
{
	static const char acFtpReplyOK[] = "250 Operation successful\r\n" ;
	static const char acFtpReplyError[] = "450 Operation error\r\n" ;
	FRESULT res;
	char databuf[128];
	char * pcFilePath = pFtpMBox->pcArg;
	char * pcPathEnd = pFtpMBox->pcArg + pFtpMBox->arglen - 1;
	vFtp_GetLegalPath(pcFilePath,pcPathEnd);

//	vFtp_LegalPath(pcFilePath);

	if (*pcFilePath != '/')//���·��
	{
		sprintf(databuf,"%s/%s",pFtpMBox->acCurrentDir,pcFilePath);
		pcFilePath = databuf;
	}
	
	printk("dele:%s\r\n",pcFilePath);

	res = f_unlink(pcFilePath);
	if (FR_OK != res)
		goto DeleError;
	
	netconn_write(pFtpMBox->pCtrlConn,acFtpReplyOK,sizeof(acFtpReplyOK)-1,NETCONN_NOCOPY);
	return ;

DeleError:	
	netconn_write(pFtpMBox->pCtrlConn,acFtpReplyError,sizeof(acFtpReplyError)-1,NETCONN_NOCOPY);

	printk("dele error code:%d\r\n",res);
	return ;
	
}


/**
	* @brief    vFtpCtrl_Cmd_STOR
	*           ftp ����˿����� STOR
	* @param    arg ������������
	* @return   NULL
*/
static void vFtpCtrl_Cmd_STOR(struct ftp_mbox * pFtpMBox)
{
	static const char acFtpReplyOK[] = "125 Waiting\r\n" ;

	netconn_write(pFtpMBox->pCtrlConn,acFtpReplyOK,sizeof(acFtpReplyOK)-1,NETCONN_NOCOPY);

	pFtpMBox->event = FTP_RECV_FILE;

	//���ʹ���Ϣ�����ݶ˿�����
	while(osMessagePut(osFtpDataPortmbox,(uint32_t)pFtpMBox , osWaitForever) != osOK);
}


/**
	* @brief    vFtpCtrlPortPro 
	*           tcp ��������������
	* @param    arg �������
	* @return   void
*/
static void vFtpCtrlPortPro(void const * arg)
{
	static const char acFtpUnknownCmd[] = "500 Unknown command\r\n";
	static const char acFtpConnect[] = "220 Operation successful\r\n";

	char *    pcRxbuf;
	uint16_t  sRxlen;
 	uint32_t iCtrlCmd ;
	
	struct netbuf   * pFtpCmdBuf;
	
	struct ftp_cmd * pCmdMatch;
	struct ftp_mbox  stFtpMBox;

	stFtpMBox.pCtrlConn = (struct netconn *)arg; //��ǰ ftp ���ƶ˿����Ӿ��	
	stFtpMBox.acCurrentDir[0] = 0;               //·��Ϊ�գ�����Ŀ¼
	
	netconn_write(stFtpMBox.pCtrlConn,acFtpConnect,sizeof(acFtpConnect)-1,NETCONN_NOCOPY);

	while(ERR_OK == netconn_recv(stFtpMBox.pCtrlConn, &pFtpCmdBuf))  //����ֱ���յ�����
	{
		do
		{
			netbuf_data(pFtpCmdBuf, (void**)&pcRxbuf, &sRxlen); //��ȡ����ָ��

			iCtrlCmd = FTP_STR2ID(pcRxbuf);
			if ( pcRxbuf[3] < 'A' || pcRxbuf[3] > 'z' )//��Щ����ֻ�������ֽڣ���Ҫ�ж�
			{
				iCtrlCmd &= 0x00ffffff;
				stFtpMBox.pcArg = pcRxbuf + 4;
				stFtpMBox.arglen = sRxlen - 4;
			}
			else
			{
				stFtpMBox.pcArg = pcRxbuf + 5;
				stFtpMBox.arglen = sRxlen - 5;
			}
			
			pCmdMatch = pFtp_SearchCmd(iCtrlCmd);//ƥ�������
			
			if (NULL == pCmdMatch)
				netconn_write(stFtpMBox.pCtrlConn,acFtpUnknownCmd,sizeof(acFtpUnknownCmd)-1,NETCONN_NOCOPY);
			else
				pCmdMatch->Func(&stFtpMBox);
		}
		while(netbuf_next(pFtpCmdBuf) >= 0);
		
		netbuf_delete(pFtpCmdBuf);
	}

	netconn_close(stFtpMBox.pCtrlConn); //�ر�����
	netconn_delete(stFtpMBox.pCtrlConn);//����ͷ����ӵ��ڴ�
	
	color_printk(green,"\r\n|!ftp disconnect!|\r\n");
	
	vTaskDelete(NULL);//���ӶϿ�ʱɾ���Լ�
}






/* Start node to be scanned (***also used as work area***) */
static char * pcFtpData_ScanDir (struct netconn * pFtpDataPortConn,struct ftp_mbox * pFtpMBox)
{
	char * pcFtpCtrlReply = (char *)acFtpCtrlMsg226;
	char   acFtpListBuf[128] ;
    DIR dir;
	FILINFO fno;

	if (FR_OK != f_opendir(&dir, pFtpMBox->acCurrentDir))
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
			vFtp_NormalList(acFtpListBuf,fno.fsize,1,1,1980,fno.fname);
		else
		if (pStDate->Year + 1980 == 2018) //ͬһ����ļ�
			vFtp_ThisYearList(acFtpListBuf,fno.fsize,pStDate->Month,pStDate->Day,pStTime->Hour,pStTime->Min,fno.fname);
		else
			vFtp_NormalList(acFtpListBuf,fno.fsize,pStDate->Month,pStDate->Day,pStDate->Year+1980,fno.fname);
		
		if (fno.fattrib & AM_DIR )   /* It is a directory */
			acFtpListBuf[0] = 'd';
		
		netconn_write(pFtpDataPortConn,acFtpListBuf,strlen(acFtpListBuf),NETCONN_COPY);
    }
	
	f_closedir(&dir); //��·���ر�

ScanDirDone:
	return pcFtpCtrlReply;
}




static char * pcFtpData_SendFile(struct netconn * pFtpDataPortConn,struct ftp_mbox * pFtpMBox)
{
	char * pcFtpCtrlReply = (char *)acFtpCtrlMsg451;
	
	FIL FileSend; 		 /* File object */
	FRESULT res ;
	uint32_t iFileSize;
	uint32_t iReadSize;
	char * pcFilePath = pFtpMBox->pcArg;
	char * pcPathEnd = pFtpMBox->pcArg + pFtpMBox->arglen - 1;
	char  databuf[128];

	vFtp_GetLegalPath(pcFilePath,pcPathEnd);

	if (*pcFilePath != '/')//���·��
	{
		sprintf(databuf,"%s/%s",pFtpMBox->acCurrentDir,pcFilePath);
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
			netconn_write(pFtpDataPortConn,databuf,iReadSize,NETCONN_COPY);
			iFileSize -= iReadSize;
		}
	}
	
	pcFtpCtrlReply = (char *)acFtpCtrlMsg226;
	f_close(&FileSend);

SendEnd:

	return pcFtpCtrlReply;//216
}



static char * pcFtpData_RecvFile(struct netconn * pFtpDataPortConn,struct ftp_mbox * pFtpMBox)
{
	static char g_acRecvBuf[TCP_MSS] ;//��Ҫ�����ݿ��������������׳���
	
	char * pcFtpCtrlReply = (char *)acFtpCtrlMsg451;
	FIL RecvFile; 		 /* File object */
	FRESULT res;
	char * pcFile = pFtpMBox->pcArg;
	char * pcPathEnd = pFtpMBox->pcArg + pFtpMBox->arglen - 1;
	char   databuf[128];
	uint16_t sRxlen;
	uint32_t byteswritten;
	struct netbuf  * pFtpDataBuf;

	vFtp_GetLegalPath(pcFile,pcPathEnd);

	if (*pcFile != '/')//���·��
	{
		sprintf(databuf,"%s/%s",pFtpMBox->acCurrentDir,pcFile);
		pcFile = databuf;
	}
	
	res = f_open(&RecvFile, pcFile, FA_CREATE_ALWAYS | FA_WRITE);
	if(res != FR_OK) 
	{
		Errors("cannot open/create \"%s\",error code = %d\r\n",pcFile,res);
		goto RecvEnd;
	}
	printk("recvfile");
	while(ERR_OK == netconn_recv(pFtpDataPortConn, &pFtpDataBuf))  //����ֱ���յ�����
	{
		do{
			netbuf_data(pFtpDataBuf, (void**)&pcFile, &sRxlen); //��ȡ����ָ��

			#if 1
			memcpy(g_acRecvBuf,pcFile,sRxlen);//�����ݿ��������������׳���
			pcFile = g_acRecvBuf;
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
		while(netbuf_next(pFtpDataBuf) >= 0);
		
		netbuf_delete(pFtpDataBuf);
	}
	
	printk("done\r\n");

	pcFtpCtrlReply = (char *)acFtpCtrlMsg226;
	f_close(&RecvFile);

RecvEnd:
	
	return pcFtpCtrlReply;
}



/**
	* @brief    vFtpDataPortPro 
	*           tcp ��������������
	* @param    arg �������
	* @return   void
*/
static void vFtpDataPortPro(void const * arg)
{	
	struct netconn * pFtpDataPortListen;
	struct netconn * pFtpDataPortConn;
	
	struct ftp_mbox * pMbox;
	char * pcCtrlReply;
	osEvent event;

	pFtpDataPortListen = netconn_new(NETCONN_TCP); //����һ�� TCP ����
	netconn_bind(pFtpDataPortListen,IP_ADDR_ANY,FTP_DATA_PORT); //�� ���ݶ˿�
	netconn_listen(pFtpDataPortListen); //�������ģʽ

	while(1)
	{
		if (ERR_OK == netconn_accept(pFtpDataPortListen,&pFtpDataPortConn)) //����ֱ���� ftp ��������
		{
			event = osMessageGet(osFtpDataPortmbox,osWaitForever);//�ȴ���������
			pMbox = (struct ftp_mbox *)(event.value.p);//��ȡ��������

			switch(pMbox->event) //���ݲ�ͬ�Ĳ���������в���
			{
				case FTP_LIST :
					pcCtrlReply = pcFtpData_ScanDir(pFtpDataPortConn,pMbox);
					break;
				
				case FTP_SEND_FILE_DATA:
					pcCtrlReply = pcFtpData_SendFile(pFtpDataPortConn,pMbox);
					break;
					
				case FTP_RECV_FILE:
					pcCtrlReply = pcFtpData_RecvFile(pFtpDataPortConn,pMbox);
					break;

				default: ;
			}

			netconn_write(pMbox->pCtrlConn,pcCtrlReply,strlen(pcCtrlReply),NETCONN_NOCOPY);//���ƶ˿ڷ���
			
			netconn_close(pFtpDataPortConn); //�ر����ݶ˿�����
			netconn_delete(pFtpDataPortConn);//����ͷ����ӵ��ڴ�
			
			printk("data port shutdown!\r\n");
		}
		else
		{
			Warnings("data port accept error\r\n");
		}
	}
		
	netconn_close(pFtpDataPortListen); //�ر�����
	netconn_delete(pFtpDataPortListen);//����ͷ����ӵ��ڴ�

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
	pfnFTPx_t vFtpCtrl_Cmd_TYPE = vFtpCtrl_Cmd_NOOP;

	//������ص����������
	vFTP_RegisterCommand(USER);
	vFTP_RegisterCommand(SYST);
	vFTP_RegisterCommand(PWD);
	vFTP_RegisterCommand(CWD);
	vFTP_RegisterCommand(PASV);
	vFTP_RegisterCommand(LIST);
	vFTP_RegisterCommand(NOOP);
	vFTP_RegisterCommand(TYPE);
	vFTP_RegisterCommand(SIZE);
	vFTP_RegisterCommand(RETR);
	vFTP_RegisterCommand(DELE);
	vFTP_RegisterCommand(STOR);
	
	osThreadDef(FtpServer, vFtp_ServerConn, osPriorityNormal, 0, 128);
	osThreadCreate(osThread(FtpServer), NULL);//��ʼ���� lwip �󴴽�tcp��������������

	osThreadDef(FtpData, vFtpDataPortPro, osPriorityNormal, 0, 512);
	osThreadCreate(osThread(FtpData), NULL);//��ʼ���� lwip �󴴽�tcp��������������

	osMessageQDef(osFtpMBox, 4,void *);
	osFtpDataPortmbox = osMessageCreate(osMessageQ(osFtpMBox),NULL);
}

