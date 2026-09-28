#ifndef RESOURCE_H
#define RESOURCE_H

#define IDI_APP                  101

/* 主窗口控件 ID */
#define IDC_LABEL_SERVER         1001
#define IDC_EDIT_SERVER          1002
#define IDC_LABEL_PORT           1003
#define IDC_EDIT_PORT            1004
#define IDC_LABEL_INTERVAL       1005
#define IDC_EDIT_INTERVAL        1006
#define IDC_LABEL_RETRY          1007
#define IDC_EDIT_RETRY           1008
#define IDC_LABEL_TIMEOUT        1009
#define IDC_EDIT_TIMEOUT         1010
#define IDC_LABEL_MAXOFF         1011
#define IDC_EDIT_MAXOFF          1012
#define IDC_LABEL_MINOFF         1013
#define IDC_EDIT_MINOFF          1014
#define IDC_CHECK_FORCE          1015
#define IDC_CHECK_SYNCONSTART    1016
#define IDC_CHECK_ALLOWLARGE     1017
#define IDC_CHECK_LOGENABLED     1018
/* 1019/1020/1021 曾用于日志路径编辑框与"浏览"按钮，界面已不再展示日志路径，
   相关控件从未创建，故一并移除以免误用。 */
#define IDC_BTN_SYNC_NOW         1022
#define IDC_BTN_SAVE             1023
#define IDC_BTN_RELOAD           1024
#define IDC_BTN_VIEW_LOG         1025
#define IDC_BTN_OPEN_DIR         1026
#define IDC_BTN_EDIT_CONFIG      1027
#define IDC_BTN_ABOUT            1028
#define IDC_BTN_MINIMIZE         1029
#define IDC_BTN_EXIT             1030
#define IDC_LABEL_STATUS         1031
#define IDC_LABEL_LASTSYNC       1032
#define IDC_LABEL_OFFSET         1033
#define IDC_LABEL_RTT            1034
#define IDC_LABEL_NEXTSYNC       1035
#define IDC_LABEL_CFG_PATH       1036
#define IDC_GROUP_STATUS         1037
#define IDC_GROUP_CONFIG         1038
#define IDC_GROUP_ACTIONS        1039
#define IDC_GROUP_SERVICE        1040
#define IDC_LABEL_SVC_STATUS     1041
#define IDC_BTN_SVC_INSTALL      1042
#define IDC_BTN_SVC_UNINSTALL    1043
#define IDC_BTN_SVC_START        1044
#define IDC_BTN_SVC_STOP         1045
#define IDC_LABEL_SVC_HINT       1046
#define IDC_BTN_SET_PWD          1047
#define IDC_BTN_CLEAR_PWD        1048
#define IDC_LABEL_PWD_STATUS     1049
#define IDC_CHECK_AUTOSTART      1050

/* 本机时钟服务 (W32Time NTP Server) */
#define IDC_GROUP_NTPSERVER      1051
#define IDC_LABEL_NTP_STATUS     1052
#define IDC_LABEL_NTP_HINT       1053
#define IDC_BTN_NTP_ENABLE       1054
#define IDC_BTN_NTP_DISABLE      1055
#define IDC_BTN_NTP_CHECK        1056

#endif /* RESOURCE_H */
