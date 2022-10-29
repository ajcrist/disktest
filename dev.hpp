class Dev {
public:
	Dev(const char*);
	~Dev();
	
	ssize_t write(uint64_t, const void*, size_t);
	ssize_t read(uint64_t, void*, size_t);
	
	bool isOpen();
	uint64_t getBlkSize();
	
	void dontNeed();
	void sync();
	
private:
	int fd = -1;
	uint64_t blkSize = 0;
};
