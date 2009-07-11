#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include <gccore.h>

#include "tmd_bin.h"
#include "tik_bin.h"
#include <wiiuse/wpad.h>			 

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

static u8 certs[0xa00] __attribute__((aligned(32)));
static const char cert_fs[] ATTRIBUTE_ALIGN(32) = "/sys/cert.sys";

#define fatal(f, a...) do { printf(f, ##a); sleep(3); exit(1); } while(0)

int check_fakesig(void)
{
	int ret;

	ret = ES_AddTitleStart((signed_blob*)tmd_bin, tmd_bin_size,
			       (signed_blob *)certs, sizeof certs,
			       NULL, 0);
	if (ret >= 0)
		ES_AddTitleCancel();

	// we don't care if IOS bitches that our version is to low because we
	// don't even want to install this title
	if (ret == -1035)
		ret = 0;
	return ret;
}

int check_identify(void)
{
	int ret;

	ret = ES_Identify((signed_blob *)certs, sizeof certs,
			  (signed_blob *)tmd_bin, tmd_bin_size,
			  (signed_blob *)tik_bin, tik_bin_size, NULL);

	// we don't care about invalid signatures here.
	// patched versions return -1017 before checking them.
	if (ret == -2011)
		ret = 0;
	return ret;
}

int check_flash(void)
{
	int ret;

	ret = IOS_Open("/dev/flash", 1);

	if (ret >= 0)
		IOS_Close(ret);

	return ret;
}

int main(int argc, char **argv)
{
	u32 n_titles;
	u32 tid;
	u64 *titles;
	int ret;
	int i;
	int j;
	int fd;
	u32 tmd_size;
	u8 *tmdbuffer;
	tmd *p_tmd;
	VIDEO_Init();
	PAD_Init();

	rmode = VIDEO_GetPreferredMode(NULL);
	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	VIDEO_ClearFrameBuffer(rmode, xfb, COLOR_BLACK);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(rmode->viTVMode&VI_NON_INTERLACE) VIDEO_WaitVSync();

	CON_InitEx(rmode, 20,30,rmode->fbWidth-40,rmode->xfbHeight-60);
	CON_EnableGecko(1, 0);
	//IOS_ReloadIOS(60);
	printf("IOSCheck v0.2, svpe,kwirky\n");
	printf("running under IOS%d %d\n", IOS_GetVersion(), IOS_GetRevision());
	printf("getting number of titles...\n");

	ret = ES_GetNumTitles(&n_titles);
	if (ret < 0)
		fatal("ES_GetNumTitles: %d\n", ret);

	printf("getting list of %d titles...\n", n_titles);
	titles = memalign(32, n_titles*sizeof(u64));
	if (titles == NULL)
		fatal("could not allocate memory for %d titles.\n", n_titles);

	ret = ES_GetTitles(titles, n_titles);
	if (ret < 0)
		fatal("ES_GetTitles: %d\n", ret);

	j = 0;
	for (i = 0; i < n_titles; i++) {
		// skip non-system titles
		if (titles[i] >> 32 != 1)
			continue;

		// skip system menu, BC, MIOS and possible other non-IOS titles
		tid = titles[i] & 0xffffffff;
		if (tid == 2)
			continue;
		if (tid > 0xff)
			continue;

		// check if this is just a stub
		ret = ES_GetStoredTMDSize(1LL<<32 | tid, &tmd_size);
		if (ret < 0)
			fatal("ES_GetStoredTMDSize(1-%d): %d\n", tid, ret);

		tmdbuffer = memalign(32, tmd_size);
		if (tmdbuffer == NULL)
			fatal("could not allocate tmdbfr (%d bytes) for 1-%d\n",
					tmd_size, tid);

		ret = ES_GetStoredTMD(1LL<<32 | tid, (signed_blob *)tmdbuffer,
				tmd_size);
		if (ret < 0)
			fatal("ES_GetStoredTMD(1-%d): %d\n", tid, ret);

		p_tmd = SIGNATURE_PAYLOAD((signed_blob *)tmdbuffer);
		if (p_tmd->num_contents == 3)
		{
			printf("IOS%4d is probably only a stub. "
					"Skipping checks.\n", tid);
			free(tmdbuffer);
			continue;
		}
		if (p_tmd->num_contents == 1)
		{
			printf("IOS%4d is only a stub. "
					"Skipping checks.\n", tid);
			free(tmdbuffer);
			continue;
		}

		free(tmdbuffer);
		// add to list
		titles[j] = titles[i];
		j++;
	}
	printf("found %d IOS titles.\n", j);

	titles = realloc(titles, j*sizeof(u64));
	n_titles = j;

	if (n_titles == 0)
		fatal("no IOS installed?!\n");

	// failsort
	u64 lowest ;
	int lowest_id;
	for (i = 0; i < n_titles; i++)
	{
		lowest = titles[i];
		lowest_id = i;
		for (j = i+1; j < n_titles; j++) {
			if (titles[j] < lowest) {
				lowest = titles[j];
				lowest_id = j;
			}
		}
		titles[lowest_id] = titles[i];
		titles[i] = lowest;
	}
	
	printf("fetching certificates from NAND...\n");

	//ISFS_Initialize();
	fd = IOS_Open(cert_fs, ISFS_OPEN_READ);
	if (fd < 0)
		fatal("IOS_Open: %d\n", fd);
	ret = IOS_Read(fd, certs, sizeof certs);
	if (ret != sizeof certs)
		fatal("IOS_Read: %d\n", ret);
	IOS_Close(fd);

	printf("legend:\n");
	printf(" IOS ??? [faked signatues] [ES_Identify] [/dev/flash] Version \n");

	for (i = 0; i < n_titles; i++) {
		if (i>0 && i%2 == 0)
			printf("\n");

		tid = titles[i] & 0xffffffff;

		//if (tid == 202) //core dump in IOS202
		//	continue;

		printf("IOS%4d: ", tid);
		fflush(stdout);

		// test fake signatures and ES_Identify
		IOS_ReloadIOS(tid);	
		if (check_fakesig() < 0)
			printf("[NO ] ");
		else
			printf("[YES] ");
		fflush(stdout);
		if (check_identify() < 0)
			printf("[NO ] ");
		else
			printf("[YES] ");
		fflush(stdout);
		if (check_flash() < 0)
			printf("[NO ] ");
		else
			printf("[YES] ");

		printf("%5d ", IOS_GetRevision());
		fflush(stdout);
	}
	
	printf("\nAll checks done. Press any button to exit.\n");
	// This function initialises the attached controllers
	WPAD_Init();
	while(1) {

		// Call WPAD_ScanPads each loop, this reads the latest controller states
		WPAD_ScanPads();

		// WPAD_ButtonsDown tells us which buttons were pressed in this loop
		// this is a "one shot" state which will not fire again until the button has been released
		u32 pressed = WPAD_ButtonsDown(0);

		// We return to the launcher application via exit
		if ( pressed /*& WPAD_BUTTON_HOME*/ )
		{
			printf("Thanks for playing...");
		    sleep(1);
		    exit(0);
		}

		// Wait for the next frame
		VIDEO_WaitVSync();
	}

	return 0;
}
