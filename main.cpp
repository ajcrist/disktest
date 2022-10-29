#define __STDC_FORMAT_MACROS
#include <cinttypes>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <climits>

#include <fcntl.h>
#include <unistd.h>

#include "rng.hpp"
#include "dev.hpp"

#define SEC_TO_NANO ((uint64_t)1'000'000'000)
#define SEC_TO_MICRO ((uint64_t)1'000'000)
#define MICRO_TO_NANO ((uint64_t)1'000)

uint64_t blocksPerChunk = 8;
uint64_t chunksPerPrint = 1024;
bool ioSync = true;
bool ioDirect = true;
bool doRead = false;
bool doWrite = false;

void seekOrDie(FILE *stream, off_t offset, int whence, const char *s) {
	if (fseeko(stream, offset, whence)) {
		perror(s);
		exit(errno);
	}
}

void reduceSiPrint(double v, bool align=true) {
	size_t i;
	const char *units = " KMGTPEZY";
	for (i=0; (i<sizeof(units)) && (v > 9999); i++) {v /= 1024.;}
	printf(align?"%4.0f":"%.0f", v);
	if (i || align) {putchar(units[i]);}
}

#define WRITELOGHEADER(F) if (F) {fputs("\n# ", F); printName(F); fputc('\n', F); fflush(F);}
#define CONVERLINES 16

template<typename GenWord_T> class TesterBase {
protected:
void testBackend(Dev &bdev) {
	uint64_t blockSize = bdev.getBlkSize();
	uint64_t chunkSize = blockSize*blocksPerChunk;
	const size_t writeChunkCount = chunkSize/sizeof(GenWord_T);
	
	uint64_t finalChunk=0, lastTimeChunk=0, ecR=0, ecW=0, tdR=0, tdW=0;
	ssize_t ret;
	struct timespec t1, t2;
	bool doLoop=true;
	
	GenWord_T *bufO=nullptr, *bufI=nullptr;
	{
		int mret;
		
		mret = posix_memalign((void**)&bufO, blockSize, chunkSize);
		if (mret) {fprintf(stderr, "Could not allocate bufO: %s\n", strerror(mret)); exit(EXIT_FAILURE);}
		
		mret = posix_memalign((void**)&bufI, blockSize, chunkSize);
		if (mret) {fprintf(stderr, "Could not allocate bufI: %s\n", strerror(mret)); exit(EXIT_FAILURE);}
	}
	
	if (!noGenInit) {
		genInit();
	} else {
		genReset();
	}
	printName(stderr); fputs(" starting:\n", stderr);
	WRITELOGHEADER(errFile);
	WRITELOGHEADER(speedFileW);
	WRITELOGHEADER(speedFileR);
	
	if (doWrite) {
		clock_gettime(CLOCK_MONOTONIC, &t1);
		for (uint64_t i=0; doLoop; i++) {
			finalChunk = i;
			genFillArray(bufO, writeChunkCount);
			
			// Write (also handle error / EOF)
			ret = bdev.write(i*blocksPerChunk, bufO, chunkSize);
			if (ret <= 0) {
				if ((ret != 0) && (errno != ENOSPC) && (errno != EINVAL)) {
					fprintf(stderr, "Fail W %08" PRIx64 ": %s\n", (chunkSize*i), strerror(errno));
					fprintf(errFile, "W\t%016" PRIx64 "\t%+08zd\t%d\n", (chunkSize*i), ret, errno); fflush(errFile);
					ecW++;
				} else {doLoop=false;}
			}
			
			// Speed test
			if ((!(i%chunksPerPrint)) || (!doLoop)) {
				clock_gettime(CLOCK_MONOTONIC, &t2);
				uint64_t thisTimeChunkSize = ((i-lastTimeChunk)*chunkSize)+((ret>0)?ret:0);
				uint64_t tdNano = ((t2.tv_sec - t1.tv_sec) * SEC_TO_NANO) + (t2.tv_nsec - t1.tv_nsec);
				tdW += tdNano / MICRO_TO_NANO;
				
				uint64_t rate = (thisTimeChunkSize*SEC_TO_MICRO)/(tdNano/MICRO_TO_NANO); // (uB/us) == (B/s) | micro instead of nano because 2^64nB ~= 18.5GB
				fputs("W ", stdout); reduceSiPrint(chunkSize*i); putchar(' '); reduceSiPrint(rate); fputs("/s        \r", stdout); fflush(stdout);
				if ((speedFileW != nullptr) && (tdNano > 0)) {
					fprintf(speedFileW, "%" PRIu64 "\t%" PRIu64 "\n", (chunkSize*i), rate);
					fflush(speedFileW);
				}
				lastTimeChunk = i;
				memcpy(&t1, &t2, sizeof(t1));
			}
		}
		putchar('\n');
	}
	
	bdev.dontNeed();
	bdev.sync();
	genReset();
	
	if (doRead) {
		doLoop = true;
		lastTimeChunk=0; clock_gettime(CLOCK_MONOTONIC, &t1);
		for (uint64_t i=0; doLoop; i++) {
			finalChunk = i;
			genFillArray(bufO, writeChunkCount);
			
			// Read (also handle error / EOF)
			for (size_t i=0; i<(chunkSize/sizeof(*bufI)); i++) {bufI[i] = i|(0xbadULL<<(64-12));}
			ret = bdev.read(i*blocksPerChunk, bufI, chunkSize);
			if (ret <= 0) {
				if ((ret != 0) && (errno != EINVAL)) {
					fprintf(stderr, "Fail R %08" PRIx64 ": %s\n", (chunkSize*i), strerror(errno));
					fprintf(errFile, "R\t%016" PRIx64 "\t%+08zd\t%d\n", (chunkSize*i), ret, errno); fflush(errFile);
					ecR++;
				} else {doLoop=false;}
			} else {
				uint_fast8_t conErrCount=0;
				size_t thisChunkWords = ret / sizeof(*bufI);
				if (thisChunkWords > writeChunkCount) {thisChunkWords = writeChunkCount;} // Probably unnecessary
				for (size_t j=0; (j<thisChunkWords); j++) {
					if (bufI[j] != bufO[j]) {
						if (conErrCount <= CONVERLINES) {
							printf("j=%zu tcw=%zu wcc=%zu\n", j, thisChunkWords, writeChunkCount);
							fprintf(stderr, "Fail V %016" PRIx64 "+%08zx %016" PRIx64 " != %016" PRIx64 "\n", (chunkSize*i), (sizeof(GenWord_T)*j), bufI[j], bufO[j]);
							if (conErrCount == CONVERLINES) {fputs("Suppressing further verification errors in this chunk\n", stderr);}
							conErrCount++;
						}
						fprintf(errFile, "V\t%016" PRIx64 "\t%08zx\t%016" PRIx64 "\t%016" PRIx64 "\n", (chunkSize*i), (sizeof(GenWord_T)*j), bufI[j], bufO[j]); fflush(errFile);
						ecR++;
					}
				}
			}
			
			// Speed test
			if ((!(i%chunksPerPrint)) || (!doLoop)) {
				clock_gettime(CLOCK_MONOTONIC, &t2);
				uint64_t thisTimeChunkSize = ((i-lastTimeChunk)*chunkSize)+((ret>0)?ret:0);
				uint64_t tdNano = ((t2.tv_sec - t1.tv_sec) * SEC_TO_NANO) + (t2.tv_nsec - t1.tv_nsec);
				tdR += tdNano / MICRO_TO_NANO;
				
				uint64_t rate = (thisTimeChunkSize*SEC_TO_MICRO)/(tdNano/MICRO_TO_NANO); // (uB/us) == (B/s) | micro instead of nano because 2^64nB ~= 18.5GB
				fputs("R ", stdout); reduceSiPrint(chunkSize*i); putchar(' '); reduceSiPrint(rate); fputs("/s        \r", stdout); fflush(stdout);
				if ((speedFileR != nullptr) && (tdNano > 0)) {
					fprintf(speedFileR, "%" PRIu64 "\t%" PRIu64 "\n", (chunkSize*i), rate);
					fflush(speedFileR);
				}
				lastTimeChunk = i;
				memcpy(&t1, &t2, sizeof(t1));
			}
		}
		putchar('\n');
	}
	
	uint64_t devSizeU = finalChunk*chunkSize;
	double devSizeD = ((double)finalChunk)*((double)chunkSize);
	
	printName(); fputs(" done:\n", stdout);
	reduceSiPrint(devSizeU, false);
	fputs(" @ ", stdout);
	
	if (doWrite) {fputs("W ", stdout); reduceSiPrint(devSizeD/((double)tdW/1e6), false); fputs("/s, ", stdout);}
	if (doRead) {fputs("R ", stdout); reduceSiPrint(devSizeD/((double)tdR/1e6), false); fputs("/s", stdout);}
	
	if (ecW || ecR) {fprintf(stdout, "\nErrors: W %" PRIu64 ", R %" PRIu64 "\n", ecW, ecR);}
	putchar('\n');
	
	free(bufO);
	free(bufI);
}
	
	bool noGenInit=false;

private:
	virtual void printName(FILE *fd=stdout) {fputs("Unnamed", fd);}
	virtual void genFillArray(GenWord_T *ptr, size_t len) {}
	virtual void genInit() {}
	virtual void genReset() {}

public:
	FILE *speedFileR=nullptr, *speedFileW=nullptr, *errFile=nullptr;
};

class TesterRng: public TesterBase<RNG_t> {
public:
	void test(Dev &bdev) {noGenInit = false; testBackend(bdev);}
	void test(Dev &bdev, RNG_t seed) {
		noGenInit = true; 
		rng.setSeed(seed);
		testBackend(bdev);
	}
	
private:
	RNG rng{};
	void printName(FILE *fd=stdout) {fprintf(fd, "Random %016" PRIx64, rng.getSeed());}
	void genFillArray(RNG_t *ptr, size_t len) {rng.fillArray(ptr, len);}
	void genInit() {rng.init();}
	void genReset() {rng.reset();}
};

class TesterSeq: public TesterBase<uint64_t> {
public:
	void test(Dev &bdev, uint64_t seq) {this->seq = seq; testBackend(bdev);}
private:
	bool haveFilled;
	uint64_t seq;
	
	void printName(FILE *fd=stdout) {fprintf(fd, "Sequence %016" PRIx64, seq);}
	void genFillArray(uint64_t *ptr, size_t len) {if (!haveFilled) {for (size_t i=0; i<len; i++) {ptr[i] = seq;}}}
	void genInit() {haveFilled = false;}
	void genReset() {}
};

uint64_t parseSeq(const char *start, const char *end) {
	uint64_t ret = 0;
	size_t len = 0;
	const char *c;
	
	for (c=start; (((!end) || (c<=end)) && (*c)); c++) {
		//printf("%p -> %p: %p = %02hhx\n", start, end, c, *c);
		if (!((*c >= '0' && *c <= '9') || ((*c >= 'A' && *c <= 'F') || (*c >= 'a' && *c <= 'f')))) {
			fprintf(stderr, "Invalid character in sequence: 0x%02hhx '%c'\n", *c, *c);
		}
	}
	len = c - start;
	
	for (size_t i=0; i<sizeof(ret)*(CHAR_BIT/4); i++) {
		size_t j = i % len;
		uint64_t nib=0;
		c = start+j;
		
		//printf("%zu -> %zu: %c\n", i, j, *c);
		if (*c >= '0' && *c <= '9') {
			nib = *c-'0';
		} else if ((*c >= 'A' && *c <= 'F') || (*c >= 'a' && *c <= 'f')) {
			nib = ((*c&0b1011111)-'A')+0xa;
		}
		ret |= nib << (((sizeof(ret)*CHAR_BIT)-4)-(4*i));
	}
	
	return ret;
}

static void printUsage(const char *progname) {
	fprintf(stderr, "Usage: %s [-airw] [-v chunks_per_print] [-b blocks_per_chunk] device [pattern]\n", progname);
	fputs("-a  Use asynchronous I/O\n", stderr);
	fputs("-i  Use indirect I/O\n", stderr);
	fputs("-r  Enable read testing\n", stderr);
	fputs("-w  Enable write testing\n", stderr);
	fputs("-v  Print status every N chunks\n", stderr);
	fputs("----\n", stderr);
	fprintf(stderr, "Using RNG %s\n", RNG::getName());
}
#define PRINTUSAGE printUsage(argc?argv[0]:"program")

int main(int argc, char *argv[]) {
	setvbuf(stdout, NULL, _IOLBF, 0); setvbuf(stderr, NULL, _IOLBF, 0);
	#ifdef DEBUG
	fputs("<!DEBUG!>\n", stderr);
	fputs("<!DEBUG!>\n", stdout);
	#endif
	
	if (argc < 2) {
		PRINTUSAGE;
		exit(1);
	}
	
	int opt;
	bool optErr=false;
	char *devName;
	while ((opt = getopt(argc, argv, "airwv:b:")) != -1) {
		switch (opt) {
			case 'a':
				fputs("Use asynchronous I/O\n", stderr);
				ioSync = false;
			break;
			
			case 'i':
				fputs("Use indirect I/O\n", stderr);
				ioDirect = false;
			break;
			
			case 'b':
				blocksPerChunk = strtoll(optarg, NULL, 0);
				if (!blocksPerChunk) {
					perror("Error parsing -b");
					exit(EXIT_FAILURE);
				}
				
				//printf("Blocks per chunk: %" PRIu64 "\n", blocksPerChunk);
			break;
			
			case 'v':
				chunksPerPrint = strtoll(optarg, NULL, 0);
				if (!chunksPerPrint) {
					perror("Error parsing -v");
					exit(EXIT_FAILURE);
				}
			break;
			
			case 'r': doRead=true; break;
			case 'w': doWrite=true; break;
			
			case ':':
				fprintf(stderr, "Option -%c requires an operand\n", optopt);
				optErr = true;
			break;
			
			case '?':
			default:
				optErr = true;
			break;
		}
		
		if (optErr) {
			PRINTUSAGE;
			exit(EXIT_FAILURE);
		}
	}
	
	if ((!doRead) && (!doWrite)) {doRead=true; doWrite=true;}
	
	if ((argc - optind) < 1) {
		fputs("No device specified\n", stderr);
		PRINTUSAGE;
		exit(EXIT_FAILURE);
	}
	
	devName = argv[optind++];
	fprintf(stderr, "Device: %s\n", devName);
	Dev bdev(devName);
	if (!bdev.isOpen()) {fputs("Could not open device\n", stderr); exit(errno);}
	
	printf("Chunk size: %" PRIu64 " * %" PRIu64 " = ", bdev.getBlkSize(), blocksPerChunk);
	reduceSiPrint(bdev.getBlkSize()*blocksPerChunk, false);
	fputs("iB\n", stdout);
	
	FILE *sfr = fopen("dt_readspeed.tsv", "a");
	if (!sfr) {perror("Error opening sfr"); exit(errno);}
	
	FILE *sfw = fopen("dt_writespeed.tsv", "a");
	if (!sfw) {perror("Error opening sfw"); exit(errno);}
	
	FILE *sfe = fopen("dt_errors.tsv", "a");
	if (!sfe) {perror("Error opening sfw"); exit(errno);}
	
	auto tester_rng = TesterRng{};
	tester_rng.errFile = sfe;
	
	auto tester_seq = TesterSeq{};
	tester_seq.speedFileR = sfr; tester_seq.speedFileW = sfw; tester_seq.errFile = sfe;
	
	if ((argc - optind) < 1) {
		tester_rng.test(bdev);
		tester_seq.test(bdev, 0);
	} else {
		char *seqStart=nullptr, *seqEnd=nullptr;
		bool seqIsRand=false;
		
		for (; optind<argc; optind++) {
			for (char *c=argv[optind]; *c; c++) {
				if (*c == '<') {
					seqStart = c+1;
					seqIsRand = ((*seqStart == 'r') || (*seqStart == 'R'));
					continue;
				}
				
				if (*c == '>') {
					seqEnd = c-1;
					if (seqStart) {
						if (!seqIsRand) {
							tester_seq.test(bdev, parseSeq(seqStart, seqEnd));
						} else {
							*c = 0; errno = 0;
							uint64_t seed = strtoll(seqStart+1, NULL, 16);
							if (errno) {
								perror("Error parsing seed");
								exit(EXIT_FAILURE);
							}
							
							tester_rng.test(bdev, seed);
						}
					} else {fputs("Unmatched '>'\n", stderr);}
					
					seqStart=nullptr; seqEnd=nullptr;
					continue;
				}
				
				if (seqStart) {continue;}
				
				if ((*c == 'r') || (*c == 'R')) {
					tester_rng.test(bdev);
					continue;
				}
				
				tester_seq.test(bdev, parseSeq(c, c));
			}
		}
	}
	
	if (fclose(sfr)) {fprintf(stderr, "Error closing sfr: %s\n", strerror(errno));}
	if (fclose(sfw)) {fprintf(stderr, "Error closing sfw: %s\n", strerror(errno));}
	if (fclose(sfe)) {fprintf(stderr, "Error closing sfe: %s\n", strerror(errno));}
	return EXIT_SUCCESS;
}
