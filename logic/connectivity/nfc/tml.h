#ifndef __TML_H
#define __TML_H

#include <stdio.h>

int tml_open(int * handle);
void tml_close(int handle);
void tml_reset(int handle);
int tml_send(int handle, char *pBuff, int buffLen);
int tml_receive(int handle, char *pBuff, int buffLen);
int tml_transceive(int handle, char *pTx, int TxLen, char *pRx, int RxLen);

#endif	// __TML_H
