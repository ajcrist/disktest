#include <sys/random.h>
#include <cstdlib>
#include <cstdio>

#include "rng.hpp"

RNG::RNG() {init();}

RNG_t* RNG::createArray(size_t len) {
	auto *ret = new RNG_t[len];
	for (size_t i=0; i<len; i++) {ret[i] = next();}
	return ret;
}

void RNG::fillArray(RNG_t *ptr, size_t len) {
	for (size_t i=0; i<len; i++) {ptr[i] = next();}
}

uint64_t RNG::getSeed() {return this->seed;}

#ifndef RNG_CHACHA_ROUNDS
void RNG::init() {
	this->val = 0;
	while (!this->val) {getrandom(&this->val, sizeof(this->val), 0);}
	seed = this->val;
}

void RNG::reset() {
	this->val = this->seed;
	if (!this->val) {
		fputs("RNG reset to zero seed\n", stderr);
		exit(1);
	}
}

void RNG::setSeed(RNG_t newSeed) {
	this->seed = this->val = newSeed;
	if (!this->val) {
		fputs("RNG seeded with zero\n", stderr);
		exit(1);
	}
}

RNG_t RNG::next() {
	val ^= val << 13;
	val ^= val >>  7;
	val ^= val << 17;
	#ifndef XORSHIFT_STAR
	return val;
	#else
	return val * 0x2545F4914F6CDD1DULL;
	#endif
}

	#ifndef XORSHIFT_STAR
		static const char *rngName = "Xorshift64";
	#else
		static const char *rngName = "Xorshift64*";
	#endif
#else
#include <cstring>

#define ROTL(a,b) (((a) << (b)) | ((a) >> (32 - (b))))
#define QR(a, b, c, d) (			\
	a += b,  d ^= a,  d = ROTL(d,16),	\
	c += d,  b ^= c,  b = ROTL(b,12),	\
	a += b,  d ^= a,  d = ROTL(d, 8),	\
	c += d,  b ^= c,  b = ROTL(b, 7))

static const char *SIGMA = "expand 32-byte k";
static const uint32_t KEY[8] = {
	0x618e5212, 0xc306369f,
	0xb2a2a253, 0xa221b269,
	0xb1982d1d, 0x32e1fb58,
	0x83af9d17, 0x5fc5f171
};

void RNG::init() {
	uint64_t x;
	getrandom(&x, sizeof(x), 0);
	this->setSeed(x);
}
void RNG::reset() {this->setSeed(this->seed);}

void RNG::setSeed(RNG_t newSeed) {
	memset(this->state, 0, sizeof(this->state));
	
	memcpy(this->state, SIGMA, 16);
	memcpy(this->state+4, &KEY, sizeof(KEY));
	memcpy(this->state+14, &newSeed, sizeof(newSeed));
	
	this->seed = newSeed;
	
#ifdef DEBUG
	puts("State:");
	for (size_t i=0; i<64; i++) {printf("%02hhx%c", ((uint8_t*)(this->state))[i], ((i%8)==7)?'\n':' ');}
	putchar('\n');
#endif
}

RNG_t RNG::next() {
	if (this->keystreamIdx >= 15) {this->chachaBlock();}
	uint64_t a = this->keystream[this->keystreamIdx++];
	uint64_t b = this->keystream[this->keystreamIdx++];
	return a|(b<<32);
}

void RNG::chachaBlock() {
	uint32_t temp[16];
	memcpy(temp, this->state, sizeof(temp));
	
	for (uint_fast8_t i=0; i<RNG_CHACHA_ROUNDS; i+=2) {
		QR(temp[0], temp[4], temp[ 8], temp[12]);
		QR(temp[1], temp[5], temp[ 9], temp[13]);
		QR(temp[2], temp[6], temp[10], temp[14]);
		QR(temp[3], temp[7], temp[11], temp[15]);
		
		QR(temp[0], temp[5], temp[10], temp[15]);
		QR(temp[1], temp[6], temp[11], temp[12]);
		QR(temp[2], temp[7], temp[ 8], temp[13]);
		QR(temp[3], temp[4], temp[ 9], temp[14]);
	}
	
	for (uint_fast8_t i=0; i<16; i++) {this->keystream[i] = temp[i]+this->state[i];}
	
	/*puts("State:");
	for (size_t i=0; i<64; i++) {printf("%02hhx%c", ((uint8_t*)(this->state))[i], ((i%8)==7)?'\n':' ');}
	
	puts("Keystream:");
	for (size_t i=0; i<64; i++) {printf("%02hhx%c", ((uint8_t*)(this->keystream))[i], ((i%8)==7)?'\n':' ');}
	putchar('\n');*/
	
	(*((uint64_t*)(this->state+12)))++; // Increment block counter
	this->keystreamIdx = 0;
}

#define DQUOTE(S) QUOTE(S)
#define QUOTE(S) #S
static const char *rngName = "ChaCha" DQUOTE(RNG_CHACHA_ROUNDS);
#endif

const char* RNG::getName() {return rngName;}