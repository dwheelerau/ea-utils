/*
Copyright (c) 2011 Expression Analysis / Erik Aronesty

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/*

See "void usage" below for usage.

*/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <math.h>
#include <sys/stat.h>
#include <search.h>

#define MAX_BARCODE_NUM 1000
#define MAX_GROUP_NUM 50
#define max(a,b) ((a)>(b)?(a):(b))
#define meminit(l) (memset(&l,0,sizeof(l)))
#define fail(s) ((fprintf(stderr,"%s",s), exit(1)))
#define endstr(e) (e=='e'?"end":e=='b'?"start":"n/a")
typedef struct line {
	char *s; int n; size_t a;
} line;

struct fq {
	line id;
	line seq;
	line com;
	line qual;
};

// barcode
struct bc {
	line id;
	line seq;
	char *out[6];			// one output per input
	FILE *fout[6];
	int cnt;			// count found
};

// group of barcodes
struct group {
	char *id;
	int tcnt;			// number of codes past thresh
	int i;				// my index
};

// barcode group
struct bcg {
	struct bc b;			// barcode
        line group;			// group (fluidigm, truseq, etc)
        int bcnt[6];			// matched begin of file n
        int ecnt[6];			// matched end of file n
	struct group *gptr;		
};

struct group* getgroup(char *s);

int read_line(FILE *in, struct line &l);		// 0=done, 1=ok, -1=err+continue
int read_fq(FILE *in, int rno, struct fq *fq);		// 0=done, 1=ok, -1=err+continue

void usage(FILE *f);
int hd(char *a, char *b, int n);
int debug=0;
// it's times like this when i think a class might be handy, but nah, not worth it
typedef struct bnode {
	char *seq;
	int cnt;
} bnode;

struct group grs[MAX_GROUP_NUM];
int grcnt=0;

struct bc bc[MAX_BARCODE_NUM+1];
int bcnt=0;

int pickmax=0;
void *picktab=NULL;
void pickbest(const void *nodep, const VISIT which, const int depth);
int bnodecomp(const void *a, const void *b) {return strcmp(((bnode*)a)->seq,((bnode*)b)->seq);};
float pickmaxpct=0.10;

int main (int argc, char **argv) {
	char c;
	bool trim = true;
	int mismatch = 1;
	char end = '\0';
	char *in[6];
	char *out[6];
	int f_n=0;
	int f_oarg=0;
	const char* guide=NULL;		// use an indexed-read
	const char* list=NULL;		// use a barcode master list
	char verify='\0';
	bool noexec = false;
	const char *group = NULL;

	int i;
	bool omode = false;	
	char *bfil = NULL;
	while (	(c = getopt (argc, argv, "-dxnbeov:m:g:l:G:")) != -1) {
		switch (c) {
		case '\1': 
                       	if (omode) {
				if (f_oarg<5)
					out[f_oarg++] = optarg;
				else {
					usage(stderr); return 1;
				}
			} else if (!bfil && !guide && !list) 
				bfil = optarg; 
			else if (f_n<5) {
				in[f_n++] = optarg; 
			} else {
				usage(stderr); return 1;
			}
			break;
                case 'o': omode=true; break;
                case 'v': 
			if (strlen(optarg)>1) {
				fprintf(stderr, "Option -v requires a single character argument");
				exit(1);
			}
			verify = *optarg; break;
		case 'b': end = 'b'; break;
		case 'e': end = 'e'; break;
		case 'G': group = optarg; break;
		case 'g': 
			guide = optarg;
			in[f_n++] = optarg;
			out[f_oarg++] = "n/a";
			break;
		case 'l': list = optarg; break;
		case 'x': trim = false; break;
		case 'n': noexec = true; break;
		case 'm': mismatch = atoi(optarg); break;
		case 'd': debug = 1; break;
		case '?': 
		     if (strchr("om", optopt))
		       fprintf (stderr, "Option -%c requires an argument.\n", optopt);
		     else if (isprint(optopt))
		       fprintf (stderr, "Unknown option `-%c'.\n", optopt);
		     else
		       fprintf (stderr,
				"Unknown option character `\\x%x'.\n",
				optopt);
		     usage(stderr);
             	     return 1;
		}
	}

	if (group && !list) {
		fprintf(stderr, "Error: -G only works with -l\n");
		return 1;
	}

	if (f_n != f_oarg) {
		fprintf(stderr, "Error: number of input files (%d) must match number of output files following '-o'.\n", f_n);
		return 1;
	}

	if (argc < 3 || !f_n || (!bfil && !guide && !list)) {
		usage(stderr);
		return 1;
	}

	FILE *fin[6];
	for (i = 0; i < f_n; ++i) {
		fin[i] = fopen(in[i], "r"); 
		if (!fin[i]) {
			fprintf(stderr, "Error opening file '%s': %s\n",in[i], strerror(errno));
			return 1;
		}
	}

	// set all to null, zero
	meminit(bc);


	// 3 ways to get barcodes
	if (list) {
		// use a list of barcode groups... determine the best set, then use the determined set 
		struct bcg bcg[MAX_GROUP_NUM * MAX_BARCODE_NUM];
		meminit(bcg);
		int bgcnt=0;
		int b;
                FILE *lin = fopen(list, "r");
                if (!lin) {
                        fprintf(stderr, "Error opening file '%s': %s\n",list, strerror(errno));
                        return 1;
                }
		int ok;
                while (bgcnt < (MAX_GROUP_NUM * MAX_BARCODE_NUM) && (ok = read_line(lin, bcg[bgcnt].b.id))) {
                        if (ok <= 0) break;
                        if (bcg[bgcnt].b.id.s[0]=='#') continue;
                        bcg[bgcnt].b.id.s=strtok(bcg[bgcnt].b.id.s, "\t\n\r ");
                        bcg[bgcnt].b.seq.s=strtok(NULL, "\t\n\r ");
                        char *g=strtok(NULL, "\n\r");
			if (!g) {
				if (bgcnt==0){
					fprintf(stderr,"Barcode guide list needs to be ID<whitespace>SEQUENCE<whitespace>GROUP");
					return 1;
				} else {
					continue;
				}
			}
			if (group) {
				if (strcasecmp(group, g)) {
					continue;
				}
			}
			bcg[bgcnt].gptr = getgroup(g);
                        bcg[bgcnt].b.id.n=strlen(bcg[bgcnt].b.id.s);
                        bcg[bgcnt].b.seq.n=strlen(bcg[bgcnt].b.seq.s);
                        if (debug) fprintf(stderr, "BCG: %d bc:%s n:%d\n", bgcnt, bcg[bgcnt].b.seq.s, bcg[bgcnt].b.seq.n);
                        ++bgcnt;
                }

		if (!bgcnt) {
			fprintf(stderr,"No barcodes %s from guide list %s.\n", group ? "matched" : "read", list);
			return 1;
		}

                int sampcnt = 20000;
                struct stat st;
		int fmax[f_n]; int bestcnt=0, besti=-1;
		meminit(fmax);
		for (i=0;i<f_n;++i) {
			stat(in[i], &st);
			fseek(fin[i], st.st_size/200 > sampcnt ? (st.st_size-sampcnt)/3 : 0, 0);
			char *s = NULL; size_t na = 0; int nr = 0, ns = 0;
			double tots=0, totsq=0;
			while (getline(&s, &na, fin[i]) > 0) {
				if (*s == '@')  {
					if ((ns=getline(&s, &na, fin[i])) <=0)
						break;
					s[--ns]='\0';
				
					for (b=0;b<bgcnt;++b) {
						if (!strncasecmp(s, bcg[b].b.seq.s, bcg[b].b.seq.n)) 
							++bcg[b].bcnt[i];
						if (!strcasecmp(s+ns-bcg[b].b.seq.n, bcg[b].b.seq.s))
							++bcg[b].ecnt[i]; 
					}	
					
					++nr;
					if (nr >= sampcnt) break;
				}
			}
			for (b=0;b<bgcnt;++b) {
				// highest count
				int hcnt = (int) (max(bcg[b].bcnt[i],bcg[b].ecnt[i]) * log(bcg[b].b.seq.n));
				if (hcnt > fmax[i]) {
					// highest count by file
					fmax[i] = hcnt;
					if (fmax[i] > bestcnt)  {
						bestcnt=fmax[i];
						besti=i;
					}
				}
			}
//			printf("max %d %d\n", i, fmax[i]);
		}
//		printf("besti: %d\n", besti);
		i=besti;

		int gmax=0, gindex=-1, scnt = 0, ecnt=0;
		int thresh = (int) (pickmaxpct*bestcnt); 
		for (b=0;b<bgcnt;++b) {
			int hcnt = (int) (max(bcg[b].bcnt[i],bcg[b].ecnt[i]) * log(bcg[b].b.seq.n));
//			printf("cnt: %s %s %d %d\n", bcg[b].b.id.s, bcg[b].b.seq.s, b, hcnt);
			if (hcnt >= thresh) {
				// increase group count	
				bcg[b].gptr->tcnt += hcnt;
				if (bcg[b].gptr->tcnt > gmax) {
					gindex=bcg[b].gptr->i;
					gmax=bcg[b].gptr->tcnt;
				}
			}
		}
//		printf("gmax: %d, gindex %d, %s, thresh: %d\n", gmax, gindex, grs[gindex].id, thresh);

                for (b=0;b<bgcnt;++b) {
			if (bcg[b].gptr->i == gindex) {
			if (bcg[b].bcnt[i] >= bcg[b].ecnt[i]) {
				++scnt;
			} else {
				++ecnt;
			}
			}
		};
		end = scnt >= ecnt ? 'b' : 'e';

		// since this is a known good set, use a very low threshold, just to catch them all
                fprintf(stderr, "Using Barcode Group: %s on File: %s (%s), Threshold %2.2f%%\n", grs[gindex].id, in[i], endstr(end), 100.0 * (float) ((float)thresh/6)/sampcnt);
                for (b=0;b<bgcnt;++b) {
			if (bcg[b].gptr->i == gindex) {
				int cnt = end == 'e' ? bcg[b].ecnt[i] : bcg[b].bcnt[i];
				if (cnt > thresh / 6) {
					bc[bcnt++]=bcg[b].b;
				}
			}
		}

		if (i != 0) {
			// in[0] needs to be the guide file
			FILE *f = fin[0];
			char *n = in[0];
			char *o = out[0];
			fin[0]=fin[i];
			in[0]=in[i];
			out[0]=out[i];
			fin[i]=f;
			in[i]=n;
			out[i]=o;
		}
	} else if (guide) {
		// use the first file as a "guide file" ... and select a set of codes from that
		FILE *gin = fin[0];

		int blen = 0;
	
                int sampcnt = 20000;
                struct stat st;
                stat(guide, &st);

		// maybe modularize a random sampling function
                fseek(gin, st.st_size/200 > sampcnt ? (st.st_size-sampcnt)/3 : 0, 0);
                char *s = NULL; size_t na = 0; int nr = 0, ns = 0;

		// small sample to get lengths
		double tots=0, totsq=0;
                while (getline(&s, &na, gin) > 0) {
                        if (*s == '@')  {
                                if ((ns=getline(&s, &na, gin)) <=0)
                                        break;
				--ns;
				tots+=ns;
				totsq+=ns*ns;
				++nr;
				if (nr >= 100) break;
			}
		}
		double dev = sqrt(((double)nr*totsq-pow(tots,2)) / ((double)nr*(nr-1)) );

		// short, and nonvarying (by much, depends on the tech used)
		if (dev < .25 && roundl(tots/nr) < 10) {
			// most probably a barcode-only read
			blen = (int) round(tots/nr);
			end = 'b';
		} else if (round(tots/nr) < 10) {
			fprintf(stderr, "File %s looks to be barcode-only, but it's length deviation is too high (%.4g)\n", in[0], dev);
			return 1;
		}

		fprintf(stderr, "Barcode length used: %d (%s)\n", blen, endstr(end));

                fseek(gin, st.st_size/200 > sampcnt ? (st.st_size-sampcnt)/3 : 0, 0);

		// load a table of possble codes
		pickmax=0;
		picktab=NULL;
		bnode * ent = NULL;
                while (getline(&s, &na, gin) > 0) {
                        if (*s == '@')  {
                                if ((ns=getline(&s, &na, gin)) <=0)
                                        break;
                                --ns;                           // don't count newline for read len
                                ++nr;

				char *p;
				if (end == 'b') {
					p=s;
				} else {
					p=s+nr-blen;
				}
				p[blen]='\0';
				if (!ent)		// make a new ent 
					ent = (bnode *) malloc(sizeof(*ent));

				if (strchr(p, 'N')||strchr(p, 'n'))
					continue;

				ent->cnt=0;
				strcpy(ent->seq=(char*)malloc(strlen(p)+1), p);

				bnode *fent = * (bnode**)  tsearch(ent, &picktab, bnodecomp);

				if (fent == ent)	// used the ent, added to tree
					ent = NULL;	// need a new one

				++fent->cnt;

				if (fent->cnt > pickmax) pickmax=fent->cnt;
			}
			if (nr > sampcnt)
				break;
		}
		pickmax=max(1,(int)(pickmaxpct*pickmax));
		fprintf(stderr, "Threshold used: %d\n", pickmax);
		twalk(picktab, pickbest);
		fseek(fin[0],0,0);
	} else {
		// user specifies a list of barcodes
		FILE *bin = fopen(bfil, "r");
		if (!bin) {
			fprintf(stderr, "Error opening file '%s': %s\n",bfil, strerror(errno));
			return 1;
		}


		bcnt = 0;
		int ok;
		while (bcnt < MAX_BARCODE_NUM && (ok = read_line(bin, bc[bcnt].id))) {
			if (ok <= 0) break;
			if (bc[bcnt].id.s[0]=='#') continue;
			bc[bcnt].id.s=strtok(bc[bcnt].id.s, "\t\n\r ");
			bc[bcnt].seq.s=strtok(NULL, "\t\n\r ");
			if (!bc[bcnt].seq.s) {
				fprintf(stderr, "Barcode file '%s' required format is 'ID SEQ'\n",bfil);
				return 1;
			}
			bc[bcnt].id.n=strlen(bc[bcnt].id.s);
			bc[bcnt].seq.n=strlen(bc[bcnt].seq.s);
			if (debug) fprintf(stderr, "BC: %d bc:%s n:%d\n", bcnt, bc[bcnt].seq.s, bc[bcnt].seq.n);
			++bcnt; 
		}

	}

	if (bcnt == 0) { 
		fprintf(stderr, "No barcodes defined, quitting.\n");
		exit(1);
	}

	if (noexec) {
		int b;
        	for (b=0;b<bcnt;++b) {
			fprintf(stdout, "%s %s\n", bc[b].id.s, bc[b].seq.s);
		}
		exit(0);
	}

	bc[bcnt].id.s="unmatched";

	int b;
        for (b=0;b<=bcnt;++b) {
		for (i=0;i<f_n;++i) {
			if (!strcasecmp(out[i],"n/a")) {
				bc[b].out[i] = NULL;
				bc[b].fout[i] = NULL;
				continue;
			}
			char *p=strchr(out[i],'%');
			if (!p) fail("Each output file name must contain a '%' sign, which is replaced by the barcode id\n");
			bc[b].out[i]=(char *) malloc(strlen(out[i])+strlen(bc[b].id.s)+10);
			strncpy(bc[b].out[i], out[i], p-out[i]);
			strcat(bc[b].out[i], bc[b].id.s);
			strcat(bc[b].out[i], p+1);
			if (!(bc[b].fout[i]=fopen(bc[b].out[i], "w"))) {
				fprintf(stderr, "Error opening output file '%s': %s\n",bc[b].out[i], strerror(errno));
				return 1;
			}
		}
	}

	// for whatever reason, the end is not supplied... easy enough to determine accurately
	if (end == '\0') {
		int sampcnt = 10000;
		struct stat st;
		stat(in[0], &st);
		fseek(fin[0], st.st_size > sampcnt/200 ? (st.st_size-sampcnt)/3 : 0, 0);
		char *s = NULL; size_t na = 0; int nr = 0, ns = 0;
		int ne=0, nb=0;
		while (getline(&s, &na, fin[0]) > 0) {
			if (*s == '@')  {
				if ((ns=getline(&s, &na, fin[0])) <=0) 
					break;
				--ns;				// don't count newline for read len
				++nr;
				for (i=0;i<bcnt;++i) {
					int d=strncmp(s, bc[i].seq.s, bc[i].seq.n);
					if (d) {
						d=strncmp(s+ns-bc[i].seq.n, bc[i].seq.s, bc[i].seq.n);
						if (!d) {
							++ne;
							break;
						}
					} else {
						++nb;
						break;
					}
				}
			}
			if (nr >= sampcnt) 
				break;
		}
		end = (ne > nb) ? 'e' : 'b';
		fseek(fin[0],0,0);
		fprintf(stderr, "End used: %s\n", endstr(end));
	}

	{
		// some basic validation of the file formats
		for (i=0;i<f_n;++i) {
			char *s = NULL; size_t na = 0; int nr = 0, ns = 0;
			fseek(fin[i], 0, 0);
			ns=getline(&s, &na, fin[i]); --ns;
			if (*s != '@')  {
				fprintf(stderr, "%s doesn't appear to be a fastq file", in[i]);
				return 1;
			}
			//fclose(fin[i]);
			//fin[i] = fopen(in[i], "r"); 
			fseek(fin[i],0,0);
		}
	}

	struct fq fq[6];	
        meminit(fq);

	int nrec=0;
	int nerr=0;
	int nok=0;
	int ntooshort=0;
	int ntrim=0;
	int nbtrim=0;
	int read_ok;

	// read in 1 record from EACH file supplied
	while (read_ok=read_fq(fin[0], nrec, &fq[0])) {
		for (i=1;i<f_n;++i) {
			int mate_ok=read_fq(fin[i], nrec, &fq[i]);
			if (read_ok != mate_ok) {
				fprintf(stderr, "# of rows in mate file '%s' doesn't match primary file, quitting!\n", in[i]);
				return 1;
			}
			if (verify) {
				// verify 1 in 100
				if (0 == (nrec % 100)) {
					char *p=strchr(fq[i].id.s,verify);
					if (!p) {
						fprintf(stderr, "File %s is missing id verification char %c at line %d", in[i], verify, nrec*4+1);
						return 1;
					}
					int l = p-fq[i].id.s;
					if (strncmp(fq[0].id.s, fq[0].id.s, l)) {
						fprintf(stderr, "File %s, id doesn't match file %s at line %d", in[0], in[i], nrec*4+1);
						return 1;
					}
				}
			}
		}
		++nrec;
		if (read_ok < 0) continue;

		if (debug) fprintf(stderr, "seq: %s %d\n", fq[0].seq.s, fq[0].seq.n);

		int i, best=-1, bestmm=mismatch+1;
		for (i =0; i < bcnt; ++i) {
                        int d;
			if (end == 'e') {
                                d=hd(fq[0].seq.s+fq[0].seq.n-bc[i].seq.n, bc[i].seq.s, bc[i].seq.n);
			} else {
				d=hd(fq[0].seq.s,bc[i].seq.s, bc[i].seq.n);
				if (debug) fprintf(stderr, "index: %d dist: %d bc:%s n:%d\n", i, d, bc[i].seq.s, bc[i].seq.n);
			}
			if (d==0) { 
				if (debug) fprintf(stderr, "found bc: %d bc:%s n:%d\n", i, bc[i].seq.s, bc[i].seq.n);
				best=i; 
				break; 
			} else if (d <= mismatch) {
				if (d == bestmm) {
					best=-1;		// more than 1 match... bad
				} else if (d < bestmm) {
					bestmm=d;		// best match...ok
					best=i;
				}
			}
		}

		if (trim) {
			int len=bc[best].seq.n;
			if (end =='b') {
				memmove(fq[0].seq.s, fq[0].seq.s+len, fq[0].seq.n-len);
				memmove(fq[0].qual.s, fq[0].qual.s+len, fq[0].seq.n-len);
			} else {
				fq[0].seq.s[fq[0].seq.n-len]='\0';
				fq[0].qual.s[fq[0].qual.n-len]='\0';
			}
		}

		if (best < 0) {
			best=bcnt;
		}

		if (debug) fprintf(stderr, "best: %d %s\n", best, bc[best].id.s);
		++bc[best].cnt;

		for (i=0;i<f_n;++i) {
			FILE *f=bc[best].fout[i];
			if (!f) continue;
                	fputs(fq[i].id.s,f);
                        fputs(fq[i].seq.s,f);
                        fputc('\n',f);
                        fputs(fq[i].com.s,f);
                        fputs(fq[i].qual.s,f);
                        fputc('\n',f);
		}
	}

	int j;
	printf("Id\tCount\tFile(s)\n");
	int tot=0;
	for (i=0;i<=bcnt;++i) {
		printf("%s\t%d", bc[i].id.s, bc[i].cnt);
		tot+=bc[i].cnt;
		for (j=0;j<f_n;++j) {
			if (bc[i].out[j])
				printf("\t%s", bc[i].out[j]);
		}
		printf("\n");
	}
	printf("total\t%d\n", tot);
	return 0;
}

struct group* getgroup(char *s) {
	int i;
	for (i=0;i<grcnt;++i) {
		if (!strcasecmp(s,grs[i].id)) {
			return &grs[i];
		}
	}
	grs[grcnt].id=s;
	grs[grcnt].tcnt=0;
	grs[grcnt].i=grcnt;
	return &grs[grcnt++];
}

void pickbest(const void *nodep, const VISIT which, const int depth)
{
	if (which==endorder || which==leaf) {
		bnode *ent = *(bnode **) nodep;
		// printf("HERE!! %s, %d, %d\n", ent->seq, ent->cnt, pickmax);
		// allow one sample to be as much as 1/10 another, possibly too conservative
		if (ent->cnt > pickmax && bcnt < MAX_BARCODE_NUM) {
			bc[bcnt].seq.s=ent->seq;
			bc[bcnt].id.s=ent->seq;
			bc[bcnt].id.n=strlen(bc[bcnt].id.s);
			bc[bcnt].seq.n=strlen(bc[bcnt].seq.s);
			++bcnt;
		} else {
			//free(ent->seq);
		}
		//free(ent);
		//tdelete((void*)ent, &picktab, scompare);
	}
}

// returns number of differences
inline int hd(char *a, char *b, int n) {
	int d=0;
	//if (debug) fprintf(stderr, "hd: %s,%s ", a, b);
	while (*a && *b && n > 0) {
		if (*a != *b) ++d;
		--n;
		++a;
		++b;
	}
	//if (debug) fprintf(stderr, ", %d/%d\n", d, n);
	return d+n;
}

int read_line(FILE *in, struct line &l) {
	return (l.n = getline(&l.s, &l.a, in));
}

int read_fq(FILE *in, int rno, struct fq *fq) {
        read_line(in, fq->id);
        read_line(in, fq->seq);
        read_line(in, fq->com);
        read_line(in, fq->qual);

        if (fq->qual.n <= 0)
                return 0;
        if (fq->id.s[0] != '@' || fq->com.s[0] != '+' || fq->seq.n != fq->qual.n) {
                fprintf(stderr, "Malformed fastq record at line %d\n", rno*2+1);
                return -1;
        }
	// chomp
	fq->seq.s[--fq->seq.n] = '\0';
	if (fq->seq.s[fq->seq.n-1] == '\r') {
		fq->seq.s[--fq->seq.n] = '\0';
	}
	fq->qual.s[--fq->qual.n] = '\0';
	if (fq->qual.s[fq->qual.n-1] == '\r') {
		fq->qual.s[--fq->qual.n] = '\0';
	}
        return 1;
}

void usage(FILE *f) {
	fputs( 
"Usage: fastq-multx [-g|-l] <barcodes.fil> <read1.fq> -o r1.%.fq [mate.fq -o r2.%.fq] ...\n"
"\n"
"Output files must contain a '%' sign which is replaced with the barcode id in the barcodes file.\n"
"\n"
"Barcodes file looks like this:\n"
"\n"
"<id1> <sequence1>\n"
"<id2> <sequence2> ...\n"
"\n"
"Default is to guess the -bol or -eol based on clear stats.\n"
"\n"
"If -g is used, then it's parameter is an index lane, and frequently occuring sequences are used.\n"
"\n"
"If -l is used then all barcodes in the file are tried, and the *group* with the *most* matches is chosen.\n" 
"\n"
"Grouped barcodes file looks like this:\n"
"\n"
"<id1> <sequence1> <group1>\n"
"<id1> <sequence1> <group1>\n"
"<id2> <sequence2> <group2>...\n"
"\n"
"Mated reads, if supplied, are kept in-sync\n"
"\n"
"Options:\n"
"\n"
"-o FIL1 [FIL2]	Output files (one per input, required)\n"
"-g FIL		Determine barcodes from indexed read FIL\n"
"-l FIL		Determine barcodes from any read, using FIL as a master list\n"
"-b		Force beginning of line\n"
"-e		Force end of line\n"
"-G NAME	Group(s) matching NAME only\n"
"-x		Don't trim barcodes before writing\n"
"-n		Don't execute, just print likely barcode list\n"
"-v C		Verify that mated id's match up to character C ('/' for illumina)\n"
"-m N		Allow up to N mismatches, as long as they are unique\n"
	,f);
}

/*
#!/usr/bin/perl

my ($f, $a) = @ARGV;

my @a = split(/,/, $a);

open (F, $f) || die;

while (my $r = <F>) {
	for my $a (@a) {
		for (my $i = 1; $i < length($a); ++$i) {
			
		}
	}
}
# http://www.perlmonks.org/?node_id=500235
sub hd{ length( $_[ 0 ] ) - ( ( $_[ 0 ] ^ $_[ 1 ] ) =~ tr[\0][\0] ) }
*/
