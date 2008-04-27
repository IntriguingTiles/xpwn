#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "dmg.h"
#include "filevault.h"

#ifdef HAVE_CRYPT

#include <openssl/hmac.h>
#include <openssl/aes.h>

#define CHUNKNO(oft) ((uint32_t)((oft)/FILEVAULT_CHUNK_SIZE))
#define CHUNKOFFSET(oft) ((size_t)((oft) - ((off_t)(CHUNKNO(oft)) * (off_t)FILEVAULT_CHUNK_SIZE)))
#define CHUNKBEGIN(oft) C_OFFSET(CHUNKNO(oft))



static void flipFileVaultV2Header(FileVaultV2Header* header) {
	FLIPENDIAN(header->signature);
	FLIPENDIAN(header->version);
	FLIPENDIAN(header->encIVSize);
	FLIPENDIAN(header->unk1);
	FLIPENDIAN(header->unk2);
	FLIPENDIAN(header->unk3);
	FLIPENDIAN(header->unk4);
	FLIPENDIAN(header->unk5);
	FLIPENDIAN(header->unk5);

	FLIPENDIAN(header->blockSize);
	FLIPENDIAN(header->dataSize);
	FLIPENDIAN(header->dataOffset);
	FLIPENDIAN(header->kdfAlgorithm);
	FLIPENDIAN(header->kdfPRNGAlgorithm);
	FLIPENDIAN(header->kdfIterationCount);
	FLIPENDIAN(header->kdfSaltLen);
	FLIPENDIAN(header->blobEncIVSize);
	FLIPENDIAN(header->blobEncKeyBits);
	FLIPENDIAN(header->blobEncAlgorithm);
	FLIPENDIAN(header->blobEncPadding);
	FLIPENDIAN(header->blobEncMode);
	FLIPENDIAN(header->encryptedKeyblobSize);
}

static void writeChunk(FileVaultInfo* info) {
	unsigned char buffer[FILEVAULT_CHUNK_SIZE];
	unsigned char buffer2[FILEVAULT_CHUNK_SIZE];
	unsigned char msgDigest[FILEVAULT_MSGDGST_LENGTH];
	uint32_t msgDigestLen;
	uint32_t myChunk;

	myChunk = info->curChunk;

	FLIPENDIAN(myChunk);
	HMAC_Init_ex(&(info->hmacCTX), NULL, 0, NULL, NULL);
	HMAC_Update(&(info->hmacCTX), (void *) &myChunk, sizeof(uint32_t));
	HMAC_Final(&(info->hmacCTX), msgDigest, &msgDigestLen);

	AES_cbc_encrypt(info->chunk, buffer, FILEVAULT_CHUNK_SIZE, &(info->aesEncKey), msgDigest, AES_ENCRYPT);

	info->file->seek(info->file, (info->curChunk * FILEVAULT_CHUNK_SIZE) + info->dataOffset);
	info->file->read(info->file, buffer2, FILEVAULT_CHUNK_SIZE);

	info->file->seek(info->file, (info->curChunk * FILEVAULT_CHUNK_SIZE) + info->dataOffset);
	info->file->write(info->file, buffer, FILEVAULT_CHUNK_SIZE);

	info->dirty = FALSE;
}

static void cacheChunk(FileVaultInfo* info, uint32_t chunk) {
	unsigned char buffer[FILEVAULT_CHUNK_SIZE];
	unsigned char buffer2[FILEVAULT_CHUNK_SIZE];
	unsigned char msgDigest[FILEVAULT_MSGDGST_LENGTH];
	unsigned char msgDigest2[FILEVAULT_MSGDGST_LENGTH];
	uint32_t msgDigestLen;

	if(chunk == info->curChunk) {
		return;
	}

	if(info->dirty) {
		writeChunk(info);
	}

	info->file->seek(info->file, chunk * FILEVAULT_CHUNK_SIZE + info->dataOffset);
	info->file->read(info->file, buffer, FILEVAULT_CHUNK_SIZE);

	info->curChunk = chunk;

	FLIPENDIAN(chunk);
	HMAC_Init_ex(&(info->hmacCTX), NULL, 0, NULL, NULL);
	HMAC_Update(&(info->hmacCTX), (void *) &chunk, sizeof(uint32_t));
	HMAC_Final(&(info->hmacCTX), msgDigest, &msgDigestLen);

	AES_cbc_encrypt(buffer, info->chunk, FILEVAULT_CHUNK_SIZE, &(info->aesKey), msgDigest, AES_DECRYPT);
}

size_t fvRead(AbstractFile* file, void* data, size_t len) {
	size_t lengthInCurrentChunk;
	FileVaultInfo* info;
	size_t toRead;

	info = (FileVaultInfo*) (file->data);

	if((CHUNKOFFSET(info->offset) + len) > FILEVAULT_CHUNK_SIZE) {
		toRead = FILEVAULT_CHUNK_SIZE - CHUNKOFFSET(info->offset);
		memcpy(data, (void *)((uint64_t)(&(info->chunk)) + (uint64_t)CHUNKOFFSET(info->offset)), toRead);
		info->offset += toRead;
		cacheChunk(info, CHUNKNO(info->offset));
		return toRead + fvRead(file, (void *)((uint64_t)data + (uint64_t)toRead), len - toRead);
	} else {
		toRead = len;
		memcpy(data, (void *)((uint64_t)(&(info->chunk)) + (uint64_t)CHUNKOFFSET(info->offset)), toRead);
		info->offset += toRead;
		cacheChunk(info, CHUNKNO(info->offset));
		return toRead;
	}
}

size_t fvWrite(AbstractFile* file, const void* data, size_t len) {
	size_t lengthInCurrentChunk;
	FileVaultInfo* info;
	size_t toRead;
	int i;

	info = (FileVaultInfo*) (file->data);

	if(info->dataSize < (info->offset + len)) {
		if(info->version == 2) {
			info->header.v2.dataSize = info->offset + len;
		}
		info->headerDirty = TRUE;
	}

	if((CHUNKOFFSET(info->offset) + len) > FILEVAULT_CHUNK_SIZE) {
		toRead = FILEVAULT_CHUNK_SIZE - CHUNKOFFSET(info->offset);
		for(i = 0; i < toRead; i++) {
			ASSERT(*((char*)((uint64_t)(&(info->chunk)) + (uint64_t)CHUNKOFFSET(info->offset) + i)) == ((char*)data)[i], "blah");
		}
		memcpy((void *)((uint64_t)(&(info->chunk)) + (uint64_t)CHUNKOFFSET(info->offset)), data, toRead);
		info->dirty = TRUE;
		info->offset += toRead;
		cacheChunk(info, CHUNKNO(info->offset));
		return toRead + fvWrite(file, (void *)((uint64_t)data + (uint64_t)toRead), len - toRead);
	} else {
		toRead = len;
		for(i = 0; i < toRead; i++) {
			ASSERT(*((char*)((uint64_t)(&(info->chunk)) + (uint64_t)CHUNKOFFSET(info->offset) + i)) == ((char*)data)[i], "blah");
		}
		memcpy((void *)((uint64_t)(&(info->chunk)) + (uint64_t)CHUNKOFFSET(info->offset)), data, toRead);
		info->dirty = TRUE;
		info->offset += toRead;
		cacheChunk(info, CHUNKNO(info->offset));
		return toRead;
	}
}

int fvSeek(AbstractFile* file, off_t offset) {
	FileVaultInfo* info = (FileVaultInfo*) (file->data);
	info->offset = offset;
	cacheChunk(info, CHUNKNO(offset));
	return 0;
}

off_t fvTell(AbstractFile* file) {
	FileVaultInfo* info = (FileVaultInfo*) (file->data);
	return info->offset;
}

off_t fvGetLength(AbstractFile* file) {
	FileVaultInfo* info = (FileVaultInfo*) (file->data);
	return info->dataSize;
}

void fvClose(AbstractFile* file) {
	FileVaultInfo* info = (FileVaultInfo*) (file->data);

	/* force a flush */
	if(info->curChunk == 0) {
		cacheChunk(info, 1);
	} else {
		cacheChunk(info, 0);
	}

	HMAC_CTX_cleanup(&(info->hmacCTX));

	if(info->headerDirty) {
		if(info->version == 2) {
			file->seek(file, 0);
			flipFileVaultV2Header(&(info->header.v2));
			file->write(file, &(info->header.v2), sizeof(FileVaultV2Header));
		}
	}

	info->file->close(info->file);
	free(info);
	free(file);
}

AbstractFile* createAbstractFileFromFileVault(AbstractFile* file, const char* key) {
	FileVaultInfo* info;
	AbstractFile* toReturn;
	uint64_t signature;
	uint8_t aesKey[16];	
	uint8_t hmacKey[20];
	
	int i;
	
	file->seek(file, 0);
	file->read(file, &signature, sizeof(uint64_t));
	FLIPENDIAN(signature);
	if(signature != FILEVAULT_V2_SIGNATURE) {
		/* no FileVault v1 handling yet */
		return NULL;
	}

	toReturn = (AbstractFile*) malloc(sizeof(AbstractFile));	
	info = (FileVaultInfo*) malloc(sizeof(FileVaultInfo));

	info->version = 2;

	file->seek(file, 0);
	file->read(file, &(info->header.v2), sizeof(FileVaultV2Header));
	flipFileVaultV2Header(&(info->header.v2));
	
	for(i = 0; i < 16; i++) {
		sscanf(&(key[i * 2]), "%02hhx", &(aesKey[i]));
	}

	for(i = 0; i < 20; i++) {
		sscanf(&(key[(16 * 2) + i * 2]), "%02hhx", &(hmacKey[i]));
	}

	HMAC_CTX_init(&(info->hmacCTX));
	HMAC_Init_ex(&(info->hmacCTX), hmacKey, sizeof(hmacKey), EVP_sha1(), NULL);
	AES_set_decrypt_key(aesKey, FILEVAULT_CIPHER_KEY_LENGTH * 8, &(info->aesKey));
	AES_set_encrypt_key(aesKey, FILEVAULT_CIPHER_KEY_LENGTH * 8, &(info->aesEncKey));

	info->dataOffset = info->header.v2.dataOffset;
	info->dataSize = info->header.v2.dataSize;
	info->offset = 0;
	info->file = file;

	info->headerDirty = FALSE;
	info->dirty = FALSE;
	info->curChunk = 1; /* just to set it to a value not 0 */
	cacheChunk(info, 0);

	toReturn->data = info;
	toReturn->read = fvRead;
	toReturn->write = fvWrite;
	toReturn->seek = fvSeek;
	toReturn->tell = fvTell;
	toReturn->getLength = fvGetLength;
	toReturn->close = fvClose;
	return toReturn;
}

#else

AbstractFile* createAbstractFileFromFileVault(AbstractFile* file, const char* key) {
	return NULL;
}

#endif