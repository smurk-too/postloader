/*

from waninkoko wadmanager 1.7

*/

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <ogcsys.h>
#include "globals.h"

#include "title.h"
#include "wad.h"
#define round_up(x,n)	(-(-(x) & -(n)))

/* 'WAD Header' structure */
typedef struct {
	/* Header length */
	u32 header_len;

	/* WAD type */
	u16 type;

	/* Padding */
	u16 padding;

	/* Data length */
	u32 certs_len;
	u32 crl_len;
	u32 tik_len;
	u32 tmd_len;
	u32 data_len;
	u32 footer_len;
} ATTRIBUTE_PACKED wadHeader;

/* Variables */
static u8 wadBuffer[BLOCK_SIZE] ATTRIBUTE_ALIGN(32);


s32 __Wad_ReadFile(FILE *fp, void *outbuf, u32 offset, u32 len)
{
	s32 ret;

	/* Seek to offset */
	fseek(fp, offset, SEEK_SET);

	/* Read data */
	ret = fread(outbuf, len, 1, fp);
	if (ret < 0)
		return ret;

	return 0;
}

s32 __Wad_ReadAlloc(FILE *fp, void **outbuf, u32 offset, u32 len)
{
	void *buffer = NULL;
	s32   ret;

	/* Allocate memory */
	buffer = memalign(32, len);
	if (!buffer)
		return -1;

	/* Read file */
	ret = __Wad_ReadFile(fp, buffer, offset, len);
	if (ret < 0) {
		free(buffer);
		return ret;
	}

	/* Set pointer */
	*outbuf = buffer;

	return 0;
}

s32 __Wad_GetTitleID(FILE *fp, wadHeader *header, u64 *tid)
{
	signed_blob *p_tik    = NULL;
	tik         *tik_data = NULL;

	u32 offset = 0;
	s32 ret;

	/* Ticket offset */
	offset += round_up(header->header_len, 64);
	offset += round_up(header->certs_len,  64);
	offset += round_up(header->crl_len,    64);

	/* Read ticket */
	ret = __Wad_ReadAlloc(fp, (void *)&p_tik, offset, header->tik_len);
	if (ret < 0)
		goto out;

	/* Ticket data */
	tik_data = (tik *)SIGNATURE_PAYLOAD(p_tik);

	/* Copy title ID */
	*tid = tik_data->titleid;

out:
	/* Free memory */
	if (p_tik)
		free(p_tik);

	return ret;
}

void __Wad_FixTicket(signed_blob *p_tik)
{
	u8 *data = (u8 *)p_tik;
	u8 *ckey = data + 0x1F1;

	if (*ckey > 1) {
		/* Set common key */
		*ckey = 0;
		
		/* Fakesign ticket */
		Title_FakesignTik(p_tik);
	}
}


s32 Wad_Install(char *filename)
{
	FILE *fp;
	
	fp = fopen (filename, "rb");
	if (!fp) 
		{
		char buff[300];
		
		sprintf (buff, "Wad file not found:\n%s", filename);
		grlib_menu (50, buff, "Ok");
		}
	
	wadHeader   *header  = NULL;
	signed_blob *p_certs = NULL, *p_crl = NULL, *p_tik = NULL, *p_tmd = NULL;

	tmd *tmd_data  = NULL;

	u32 cnt, offset = 0;
	s32 ret;

	Video_WaitPanel (TEX_HGL, "Reading WAD data...");

	/* WAD header */
	ret = __Wad_ReadAlloc(fp, (void *)&header, offset, sizeof(wadHeader));
	if (ret >= 0)
		offset += round_up(header->header_len, 64);
	else
		goto err;

	/* WAD certificates */
	ret = __Wad_ReadAlloc(fp, (void *)&p_certs, offset, header->certs_len);
	if (ret >= 0)
		offset += round_up(header->certs_len, 64);
	else
		goto err;

	/* WAD crl */
	if (header->crl_len) {
		ret = __Wad_ReadAlloc(fp, (void *)&p_crl, offset, header->crl_len);
		if (ret >= 0)
			offset += round_up(header->crl_len, 64);
		else
			goto err;
	}

	/* WAD ticket */
	ret = __Wad_ReadAlloc(fp, (void *)&p_tik, offset, header->tik_len);
	if (ret >= 0)
		offset += round_up(header->tik_len, 64);
	else
		goto err;

	/* WAD TMD */
	ret = __Wad_ReadAlloc(fp, (void *)&p_tmd, offset, header->tmd_len);
	if (ret >= 0)
		offset += round_up(header->tmd_len, 64);
	else
		goto err;

	/* Fix ticket */
	__Wad_FixTicket(p_tik);

	Video_WaitPanel (TEX_HGL, "Installing ticket...");

	/* Install ticket */
	ret = ES_AddTicket(p_tik, header->tik_len, p_certs, header->certs_len, p_crl, header->crl_len);
	if (ret < 0)
		goto err;

	Video_WaitPanel (TEX_HGL, "Installing title...");

	/* Install title */
	ret = ES_AddTitleStart(p_tmd, header->tmd_len, p_certs, header->certs_len, p_crl, header->crl_len);
	if (ret < 0)
		goto err;

	/* Get TMD info */
	tmd_data = (tmd *)SIGNATURE_PAYLOAD(p_tmd);

	/* Install contents */
	for (cnt = 0; cnt < tmd_data->num_contents; cnt++) {
		tmd_content *content = &tmd_data->contents[cnt];

		u32 idx = 0, len;
		s32 cfd;

		Video_WaitPanel (TEX_HGL, "Installing content #%02d...", content->cid);

		/* Encrypted content size */
		len = round_up(content->size, 64);

		/* Install content */
		cfd = ES_AddContentStart(tmd_data->title_id, content->cid);
		if (cfd < 0) {
			ret = cfd;
			goto err;
		}

		/* Install content data */
		while (idx < len) {
			u32 size;

			/* Data length */
			size = (len - idx);
			if (size > BLOCK_SIZE)
				size = BLOCK_SIZE;

			/* Read data */
			ret = __Wad_ReadFile(fp, &wadBuffer, offset, size);
			if (ret < 0)
				goto err;

			/* Install data */
			ret = ES_AddContentData(cfd, wadBuffer, size);
			if (ret < 0)
				goto err;

			/* Increase variables */
			idx    += size;
			offset += size;
		}

		/* Finish content installation */
		ret = ES_AddContentFinish(cfd);
		if (ret < 0)
			goto err;
	}

	Video_WaitPanel (TEX_HGL, "Finishing installation...");

	/* Finish title install */
	ret = ES_AddTitleFinish();
	Debug ("ES_AddTitleFinish = %d\n");
	if (ret >= 0)
		goto out;

err:
	Video_WaitPanel (TEX_HGL, "ERROR %d", ret);
	sleep (1);

	/* Cancel install */
	ES_AddTitleCancel();

out:
	/* Free memory */
	if (header)
		free(header);
	if (p_certs)
		free(p_certs);
	if (p_crl)
		free(p_crl);
	if (p_tik)
		free(p_tik);
	if (p_tmd)
		free(p_tmd);

	return ret;
}

s32 Wad_Uninstall(char *filename)
{
	FILE *fp;
	
	fp = fopen (filename, "rb");
	if (!fp) 
		{
		char buff[300];
		
		sprintf (buff, "Wad file not found:\n%s", filename);
		grlib_menu (50, buff, "Ok");
		}
	wadHeader *header   = NULL;
	tikview   *viewData = NULL;

	u64 tid;
	u32 viewCnt;
	s32 ret;

	Video_WaitPanel (TEX_HGL, "Reading WAD data...");

	/* WAD header */
	ret = __Wad_ReadAlloc(fp, (void *)&header, 0, sizeof(wadHeader));
	if (ret < 0) {
		Debug(" __Wad_ReadAlloc (ret = %d)\n", ret);
		goto out;
	}

	/* Get title ID */
	ret =  __Wad_GetTitleID(fp, header, &tid);
	if (ret < 0) {
		Debug ("__Wad_GetTitleID = %d", ret);
		goto out;
	}

	Video_WaitPanel (TEX_HGL, "Deleting tickets...");

	/* Get ticket views */
	ret = Title_GetTicketViews(tid, &viewData, &viewCnt);
	if (ret < 0)
		{
		Debug ("Title_GetTicketViews = %d", ret);
		}

	/* Delete tickets */
	if (ret >= 0) {
		u32 cnt;

		/* Delete all tickets */
		for (cnt = 0; cnt < viewCnt; cnt++) {
			ret = ES_DeleteTicket(&viewData[cnt]);
			if (ret < 0)
				break;
		}
		
		Debug ("ES_DeleteTicket = %d", ret);
	}

	Video_WaitPanel (TEX_HGL, "Deleting title contents...");

	/* Delete title contents */
	ret = ES_DeleteTitleContent(tid);
	Debug ("ES_DeleteTitleContent = %d", ret);

	Video_WaitPanel (TEX_HGL, "Deleting title...");

	/* Delete title */
	ret = ES_DeleteTitle(tid);
	Debug ("ES_DeleteTitle = %d", ret);

out:
	/* Free memory */
	if (header)
		free(header);

	return ret;
}