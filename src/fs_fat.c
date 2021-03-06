/*
 * Dokan Image Mounter
 *
 * FAT file system.
 */

typedef enum {
	FAT_UNKNOWN,
	FAT12 = 1,
	FAT16 = 2,
	FAT32 = 3
} FAT_TYPE;

// FAT cluster number. Since we only need 28 bits, signed is better here.
typedef int32_t fat_cluster_t;

const fat_cluster_t FAT_CLUSTERS_MIN[4] = {0, 2, 0xFF7, 0xFFF7};
const fat_cluster_t FAT_CLUSTERS_MAX[4] = {0, 0xFF6, 0xFFF6, 0x0FFFFFF6};

#pragma pack(push, 1)
typedef struct {
	uint8_t DriveNumber;
	uint8_t MountState;
	uint8_t Signature;
	uint32_t Serial;
	char Label[11];
	char FSType[8];
} FAT_EXTENDED_BOOT_RECORD;

typedef struct {
	uint8_t Jump[3]; // Boot strap short or near jump
	uint8_t SystemID[8]; // Name - can be used to special case partition manager volumes
	uint16_t SecSize; // bytes per logical sector
	uint8_t SecsPerClus; // sectors/cluster
	uint16_t SecsReserved; // reserved sectors
	uint8_t	FATs; // number of FATs
	uint16_t RootDirEntries;
	uint16_t FSSectors16; // 16-bit number of sectors
	uint8_t	Media; // media code
	uint16_t FATSectors16; // sectors/FAT
	uint16_t SecsPerTrack; // sectors per track
	uint16_t Heads; // number of heads
	uint32_t SecsHidden; // hidden sectors (unused)
	uint32_t FSSectors32; // 32-bit number of sectors (if FSSectors16 == 0)

	union {
		FAT_EXTENDED_BOOT_RECORD EBPB;

		struct {
			uint32_t FATSectors32;
			uint16_t Flags;
			uint16_t Version;
			uint32_t RootDirCluster;
			uint16_t FSInfoSector;
			uint16_t BootBackupSector;
			FAT_EXTENDED_BOOT_RECORD EBPB;
		} FAT32;
	};
} FAT_BOOT_RECORD;

typedef struct {
	// Both BaseName and Extension are padded out with spaces. The first
	// character of FileName can be the following:
	// * 0x00: unused file
	// * 0x05: first character is actually 0xE5
	// * 0xE5: previously deleted file
	char BaseName[8];
	char Extension[3];
	uint8_t Attribute;
	uint8_t Reserved[10];
	uint16_t Time;
	uint16_t Date;
	uint16_t FirstCluster;
	uint32_t Size;
} FAT_DIR_ENTRY;

typedef struct {
	uint8_t Segment;
	wchar_t Name1[5];
	uint8_t Attribute;
	uint8_t Type;
	uint8_t Checksum;
	wchar_t Name2[6];
	uint16_t FirstCluster; // always 0
	wchar_t Name3[2];
} FAT_LFN_ENTRY;
#pragma pack(pop)

#define FILE_ATTRIBUTE_VOLUME_LABEL 0x08

typedef fat_cluster_t FAT_Lookup_t(void *fat, fat_cluster_t Num);

// Some precalculated filesystem constants
typedef struct {
	FAT_DIR_ENTRY *RootDir;
	VIEW Data;
	FAT_Lookup_t *Lookup;
	uint8_t **FATs;
	FAT_TYPE Type;
	fat_cluster_t ClusterChainEnd;
	uint32_t FSSectors;
	uint32_t FATSectors;
	uint32_t DataSectors;
	fat_cluster_t Clusters;
	uint32_t ClusterSize;
} FAT_INFO;

#define FBR_GET \
	const FAT_BOOT_RECORD *fbr = LStructAt(FAT_BOOT_RECORD, FS, 0);
#define FAT_INFO_GET \
	FAT_INFO *fat_info = FS->FSData;
#define FBR_GET_ASSERT \
	FBR_GET; \
	assert(fbr);

// Shared by FindFiles() and GetFileInformation(), and used for both
// WIN32_FIND_DATA and BY_HANDLE_FILE_INFORMATION structures, which is why it
// needs to be a macro.
#define FAT_FILL_FILE_INFO(Obj) \
	FILETIME timestamp; \
	DosDateTimeToFileTime(dentry->Date, dentry->Time, &timestamp); \
	(Obj)->nFileSizeHigh = 0; \
	(Obj)->nFileSizeLow = dentry->Size; \
	(Obj)->ftCreationTime = timestamp; \
	(Obj)->ftLastAccessTime = timestamp; \
	(Obj)->ftLastWriteTime = timestamp; \
	(Obj)->dwFileAttributes = dentry->Attribute;

uint8_t* FAT_AtCluster(FAT_INFO *FATInfo, fat_cluster_t Cluster)
{
	Cluster -= 2;
	if(Cluster < 2 || Cluster >= FATInfo->Clusters || Cluster == FATInfo->ClusterChainEnd) {
		return NULL;
	}
	return At(&FATInfo->Data, Cluster * FATInfo->ClusterSize, FATInfo->ClusterSize);
}

int FAT_ValidMedia(uint8_t media)
{
	return 0xf8 <= media || media == 0xf0;
}

bool FAT_ValidLFNEntry(FAT_LFN_ENTRY *Entry)
{
	assert(Entry);
	const uint8_t LFN_ATTRIB = FILE_ATTRIBUTE_VOLUME_LABEL
		| FILE_ATTRIBUTE_SYSTEM
		| FILE_ATTRIBUTE_HIDDEN
		| FILE_ATTRIBUTE_READONLY;
	return Entry->Attribute == LFN_ATTRIB
		&& (Entry->Segment & 0x20) == 0
		&& Entry->Type == 0
		&& Entry->FirstCluster == 0;
}

bool FAT_ToShortName(char *dst, const wchar_t *src_w, size_t src_len, UINT codepage)
{
	assert(dst);
	assert(src_w);
	char src_a[260];
	WideCharToMultiByte(
		codepage, 0, src_w, -1, src_a, sizeof(src_a), NULL, NULL
	);
	size_t base_len = 0;
	while(src_a[base_len] != '.' && src_a[base_len] != '\0' && base_len < src_len) {
		base_len++;
	}
	const char *ext = src_a + base_len;
	size_t ext_len = src_len - base_len;
	if(ext[0] == '.') {
		ext++;
		ext_len--;
	}
	if(base_len > 8 || ext_len > 3) {
		dst[0] = 0;
		return false;
	}
	memcpy(dst, src_a, base_len);
	memset(dst + base_len, ' ', 8 - base_len);
	memcpy(dst + 8, ext, ext_len);
	memset(dst + 8 + ext_len, ' ', 3 - ext_len);
	if((unsigned char)(dst[0]) == 0xE5) {
		dst[0] = 0x05;
	}
	return true;
}

size_t FAT_NameComponentCopy(char *dst, const char *src, size_t len)
{
	assert(dst);
	assert(src);
	len = TrimmedLength(src, len);
	memcpy(dst, src, len);
	dst[len] = '\0';
	return len;
}

fat_cluster_t FAT12_ClusterLookup(uint8_t *fat, fat_cluster_t Num)
{
	fat_cluster_t c = (Num * 3) / 2;
	fat_cluster_t ret;
	if(Num & 1) {
		ret = (fat[c] & 0xF0) >> 4 | (fat[c + 1] << 4);
	} else {
		ret = fat[c] | (fat[c + 1] & 0xF) << 8;
	}
	if(ret > FAT_CLUSTERS_MAX[FAT12]) {
		ret |= 0x0FFFF000;
	}
	return ret;
}

fat_cluster_t FAT16_ClusterLookup(uint16_t *fat, fat_cluster_t Num)
{
	fat_cluster_t ret = fat[Num];
	if(ret > FAT_CLUSTERS_MAX[FAT16]) {
		ret |= 0x0FFF0000;
	}
	return ret;
}

fat_cluster_t FAT32_ClusterLookup(uint32_t *fat, fat_cluster_t Num)
{
	return fat[Num] & 0x0FFFFFFF;
}

fat_cluster_t FAT_ClusterLookup(FAT_INFO *FI, fat_cluster_t Num)
{
	uint8_t *fat = FI->FATs[0];
	if(Num < FI->Clusters) {
		return FI->Lookup(fat, Num);
	}
	return 0;
}

/// Directory iteration
/// -------------------
typedef struct {
	fat_cluster_t Cluster;
	FAT_DIR_ENTRY *Base;
	uint32_t Index;
	uint32_t Limit;
} FAT_DIR_ITERATOR;

void FAT_DirIterateInit(FILESYSTEM *FS, FAT_DIR_ITERATOR *Iter, FAT_DIR_ENTRY *DPointer)
{
	FBR_GET_ASSERT;
	FAT_INFO_GET;
	assert(Iter);
	if(!DPointer) {
		Iter->Base = NULL;
		return;
	}
	Iter->Cluster = DPointer->FirstCluster;
	Iter->Index = 0;
	if(Iter->Cluster == 0) {
		Iter->Base = fat_info->RootDir;
		Iter->Limit = fbr->RootDirEntries;
	} else {
		Iter->Base = (FAT_DIR_ENTRY*)FAT_AtCluster(fat_info, Iter->Cluster);
		Iter->Limit = (fat_info->ClusterSize) / sizeof(FAT_DIR_ENTRY);
	}
}

FAT_DIR_ENTRY* FAT_DirIterate(FILESYSTEM *FS, FAT_DIR_ITERATOR *Iter)
{
	FAT_INFO_GET;
	assert(Iter);
	if(Iter->Base == NULL) {
		return NULL;
	}
	FAT_DIR_ENTRY *ret = &Iter->Base[Iter->Index];
	Iter->Index++;
	if(Iter->Index == Iter->Limit) {
		if(Iter->Cluster == 0) {
			// Root directory
			Iter->Base = NULL;
		} else {
			// Subdirectory
			Iter->Cluster = FAT_ClusterLookup(fat_info, Iter->Cluster);
			Iter->Base = (FAT_DIR_ENTRY*)FAT_AtCluster(fat_info, Iter->Cluster);
		}
		Iter->Index = 0;
	}
	return ret;
}
/// -------------------

const wchar_t* FS_FAT_Name(FILESYSTEM *FS)
{
	if(!FS) {
		return L"FAT";
	}
	FAT_INFO_GET;
	switch(fat_info->Type) {
	case FAT12:
		return L"FAT12";
	case FAT16:
		return L"FAT16";
	case FAT32:
		return L"FAT32";
	default:
		return L"(unknown)";
	}
}

FAT_TYPE FAT_TypeFromField(const char *Type)
{
	if(!memcmp(Type, "FAT12   ", 8)) {
		return FAT12;
	} else if(!memcmp(Type, "FAT16   ", 8)) {
		return FAT16;
	} else if(!memcmp(Type, "FAT32   ", 8)) {
		return FAT32;
	}
	return FAT_UNKNOWN;
}

FAT_TYPE FAT_TypeFromClusterCount(fat_cluster_t Count)
{
	for(FAT_TYPE i = FAT12; i <= FAT32; i++) {
		if(Count >= FAT_CLUSTERS_MIN[i] && Count <= FAT_CLUSTERS_MAX[i]) {
			return i;
		}
	}
	return FAT_UNKNOWN;
}

int FS_FAT_Probe(FILESYSTEM *FS)
{
	FAT_INFO fi = {0};
	FBR_GET;
	if(!fbr) {
		return 1;
	}
	if(!FAT_ValidMedia(fbr->Media) || fbr->FATs == 0) {
		return 1;
	}
	FS->SectorSize = fbr->SecSize;

	// Sort of reliably determine the FAT width using the algorithm from
	// http://homepage.ntlworld.com/jonathan.deboynepollard/FGA/determining-fat-widths.html

	fi.FSSectors = fbr->FSSectors16 ? fbr->FSSectors16 : fbr->FSSectors32;

	if(fbr->FAT32.EBPB.Signature == 0x28 || fbr->FAT32.EBPB.Signature == 0x29) {
		fi.FATSectors = fbr->FATSectors16 ? fbr->FATSectors16 : fbr->FAT32.FATSectors32;
		fi.Type = FAT_TypeFromField(fbr->FAT32.EBPB.FSType);
	} else {
		fi.FATSectors = fbr->FATSectors16;
		fi.Type = FAT_TypeFromField(fbr->EBPB.FSType);
	}
	uint32_t root_dir_len =
		fbr->RootDirEntries * sizeof(FAT_DIR_ENTRY) + fbr->SecSize - 1;
	uint32_t root_dir_start_sec = fbr->SecsReserved + (fi.FATSectors * fbr->FATs);
	uint32_t data_start_sec = root_dir_start_sec + (root_dir_len / fbr->SecSize);
	fi.DataSectors = fi.FSSectors - data_start_sec;
	fat_cluster_t actual_clusters = fi.DataSectors / fbr->SecsPerClus;
	fi.Clusters = 2 + actual_clusters;
	fi.RootDir = FSStructAtSector(FAT_DIR_ENTRY, FS, root_dir_start_sec);
	if(fi.Type == FAT_UNKNOWN) {
		fi.Type = FAT_TypeFromClusterCount(fi.Clusters);
		if(fi.Type == FAT_UNKNOWN) {
			return 1;
		}
	}
	uint32_t size = fi.FSSectors * FS->SectorSize;
	if(!LAt(FS, size, 0)) {
		return 1;
	}
	FS->View.Size = size;

	fi.ClusterSize = fbr->SecSize * fbr->SecsPerClus;
	fi.Data.Size = fi.ClusterSize * actual_clusters;
	fi.Data.Memory = FSAtSector(FS, data_start_sec, actual_clusters * fbr->SecsPerClus);

	fat_cluster_t max_clusters = fi.FATSectors * fbr->SecSize;
	switch(fi.Type) {
	case FAT_UNKNOWN:
		// clang wants us to handle this, but since this can't happen anyway...
		assert(NULL);
		break;
	case FAT12:
		fi.Lookup = (FAT_Lookup_t*)FAT12_ClusterLookup;
		max_clusters = (max_clusters * 2) / 3;
		FS->Serial = fbr->EBPB.Serial;
		break;
	case FAT16:
		fi.Lookup = (FAT_Lookup_t*)FAT16_ClusterLookup;
		max_clusters /= 2;
		FS->Serial = fbr->EBPB.Serial;
		break;
	case FAT32:
		fi.Lookup = (FAT_Lookup_t*)FAT32_ClusterLookup;
		max_clusters /= 4;
		FS->Serial = fbr->FAT32.EBPB.Serial;
		break;
	}
	fi.Clusters = fi.DataSectors / fbr->SecsPerClus;
	if(fi.Clusters > max_clusters) {
		return 1;
	}

	fi.FATs = HeapAlloc(GetProcessHeap(), 0, sizeof(uint8_t*) * fbr->FATs);
	if(!fi.FATs) {
		return ERROR_OUTOFMEMORY;
	}
	for(uint8_t i = 0; i < fbr->FATs; i++) {
		fi.FATs[i] = FSAtSector(
			FS, fbr->SecsReserved + (fi.FATSectors * i), fi.FATSectors
		);
	}

	FS->FSData = HeapAlloc(GetProcessHeap(), 0, sizeof(FAT_INFO));
	if(!FS->FSData) {
		return ERROR_OUTOFMEMORY;
	}
	fi.ClusterChainEnd = FAT_ClusterLookup(&fi, 1);
	memcpy(FS->FSData, &fi, sizeof(FAT_INFO));
	return 0;
}

void FS_FAT_DiskSizes(FILESYSTEM *FS, uint64_t *Total, uint64_t *Available)
{
	FAT_INFO_GET;
	*Total = fat_info->DataSectors * FS->SectorSize;
	*Available = 0;
	for(fat_cluster_t i = 2; i < fat_info->Clusters; i++) {
		if(FAT_ClusterLookup(fat_info, i) == 0) {
			*Available += fat_info->ClusterSize;
		}
	}
}

FAT_DIR_ENTRY* FAT_FileLookupRecurse(FILESYSTEM *FS, const wchar_t *NestName, FAT_DIR_ENTRY *DEntry)
{
	FAT_DIR_ENTRY* FAT_FileLookup(FILESYSTEM *FS, const wchar_t *FileName, FAT_DIR_ENTRY *DStart);

	assert(NestName);
	if(IsDirSepW(NestName[0])) {
		if(DEntry->Attribute & FILE_ATTRIBUTE_DIRECTORY) {
			return FAT_FileLookup(FS, NestName, DEntry);
		}
		return NULL;
	}
	return DEntry;
}

FAT_DIR_ENTRY* FAT_FileLookup(FILESYSTEM *FS, const wchar_t *FileName, FAT_DIR_ENTRY *DStart)
{
	// By creating a fake dentry pointing to the root directory,
	// we can always return a FAT_DIR_ENTRY from this function.
	static FAT_DIR_ENTRY fakeroot = {
		.FirstCluster = 0,
		.Attribute = FILE_ATTRIBUTE_DIRECTORY
	};

	assert(FS);
	assert(FileName);
	assert(IsDirSepW(FileName[0]));
	char fat_name[8 + 3];
	uint8_t lfn_segments = 0;
	uint8_t lfn_segments_correct = 0;
	FAT_DIR_ITERATOR iter;

	if(DStart == NULL) {
		DStart = &fakeroot;
	}
	if(FileName[1] == '\0' || FileName[0] == '\0') {
		return DStart;
	}
	FileName++;
	size_t fn_len = 0;
	while(FileName[fn_len] != '\0' && !IsDirSepW(FileName[fn_len])) {
		fn_len++;
	}
	bool sfn = FAT_ToShortName(fat_name, FileName, fn_len, FS->CodePage);
	if(!sfn) {
		lfn_segments = (uint8_t)((fn_len + 12) / 13);
	}

	FAT_DirIterateInit(FS, &iter, DStart);
	FAT_DIR_ENTRY *dentry;
	while((dentry = FAT_DirIterate(FS, &iter))) {
		if(dentry->BaseName[0] == '\0') {
			break;
		}
		if(sfn && !_strnicmp(dentry->BaseName, fat_name, sizeof(fat_name))) {
			return FAT_FileLookupRecurse(FS, FileName + fn_len, dentry);
		} else if(!sfn) {
			FAT_LFN_ENTRY *lfn_entry = (FAT_LFN_ENTRY*)dentry;
			if(lfn_segments == lfn_segments_correct) {
				return FAT_FileLookupRecurse(FS, FileName + fn_len, dentry);
			}
			uint8_t segment_local = lfn_entry->Segment & 0x1F;
			const wchar_t *sgm_lookup = FileName + ((segment_local - 1) * 13);
			if(FAT_ValidLFNEntry(lfn_entry)) {
				if((segment_local <= lfn_segments) && (lfn_segments_correct != 0xFF)) {
					wchar_t sgm_cur[14];
					memcpy(sgm_cur + 0, lfn_entry->Name1, sizeof(lfn_entry->Name1));
					memcpy(sgm_cur + 5, lfn_entry->Name2, sizeof(lfn_entry->Name2));
					memcpy(sgm_cur + 11, lfn_entry->Name3, sizeof(lfn_entry->Name3));
					sgm_cur[13] = L'\0';
					if(!_wcsnicmp(sgm_cur, sgm_lookup, wcslen(sgm_cur))) {
						lfn_segments_correct++;
					} else {
						lfn_segments_correct = 0xFF;
					}
				}
			} else {
				lfn_segments_correct = 0;
			}
		}
	}
	return NULL;
}

ULONG64 FS_FAT_FileLookupW(FILESYSTEM *FS, const wchar_t *FileName)
{
	return (ULONG64)FAT_FileLookup(FS, FileName, NULL);
}

NTSTATUS FS_FAT_FindFiles(FILESYSTEM *FS, ULONG64 Dir, FIND_CALLBACK_DATA *FCD)
{
	FAT_DIR_ITERATOR iter;
	FAT_DIR_ENTRY *dentry = (FAT_DIR_ENTRY*)Dir;
	FAT_DirIterateInit(FS, &iter, dentry);
	// Both of these are 1-based!
	uint8_t lfn_length = 0;
	uint8_t lfn_segment = 0;

	while((dentry = FAT_DirIterate(FS, &iter))) {
		WIN32_FIND_DATAA fd_a;
		WIN32_FIND_DATAW fd_w;
		FAT_LFN_ENTRY *lfn_entry = (FAT_LFN_ENTRY*)dentry;
		unsigned char first = dentry->BaseName[0];
		if(first == 0x00) {
			break;
		} else if(FAT_ValidLFNEntry(lfn_entry)) {
			if(lfn_length == 0 && (lfn_entry->Segment & 0x40)) {
				ZeroMemory(fd_w.cFileName, sizeof(fd_w.cFileName));
				lfn_length = lfn_entry->Segment & 0x1F;
				lfn_segment = lfn_length;
				if(lfn_length > 20) {
					fwprintf(stderr, L"**Warning** ignoring filename longer than 260 characters\n");
					lfn_length = 0;
				}
			}
			if(lfn_length >= 1 && lfn_length <= 20) {
				uint8_t segment_local = lfn_entry->Segment & 0x1F;
				if(segment_local != lfn_segment) {
					fwprintf(stderr,
						L"**Warning** malformed long file name; expected segment #%d, not %d\n",
						lfn_segment, segment_local
					);
				}
				if(segment_local <= 20) {
					wchar_t *sgm_dst = fd_w.cFileName + ((segment_local - 1) * 13);
					memcpy(sgm_dst + 0, lfn_entry->Name1, sizeof(lfn_entry->Name1));
					memcpy(sgm_dst + 5, lfn_entry->Name2, sizeof(lfn_entry->Name2));
					memcpy(sgm_dst + 11, lfn_entry->Name3, sizeof(lfn_entry->Name3));
					if(segment_local == 20) {
						sgm_dst[12] = L'\0';
					}
				}
				lfn_segment--;
			}
		} else if(first != 0xE5 && (dentry->Attribute & FILE_ATTRIBUTE_VOLUME_LABEL) == 0) {
			char ext[sizeof(dentry->Extension) + 1] = {0};
			FAT_NameComponentCopy(ext, dentry->Extension, sizeof(dentry->Extension));
			size_t name_len = FAT_NameComponentCopy(
				fd_a.cFileName, dentry->BaseName, sizeof(dentry->BaseName)
			);
			if(first == 0x05) {
				fd_a.cFileName[0] = 0xE5;
			}
			if(ext[0] != '\0') {
				fd_a.cFileName[name_len++] = '.';
				memcpy(&fd_a.cFileName[name_len], ext, sizeof(ext));
			}
			if(lfn_length != 0) {
				FAT_FILL_FILE_INFO(&fd_w);
				MultiByteToWideChar(
					FS->CodePage, 0, fd_a.cFileName, 14, fd_w.cAlternateFileName, sizeof(fd_w.cAlternateFileName)
				);
				FindAddFileW(FCD, &fd_w);
				lfn_length = 0;
			} else {
				FAT_FILL_FILE_INFO(&fd_a);
				FindAddFileA(FCD, &fd_a);
			}
		}
	}
	return STATUS_SUCCESS;
}

NTSTATUS FS_FAT_CreateFileW(FILESYSTEM *FS, LPCWSTR FileName, DWORD AccessMode, DWORD CreationDisposition, DWORD FlagsAndAttributes, PDOKAN_FILE_INFO DokanFileInfo)
{
	FAT_DIR_ENTRY *dentry = FAT_FileLookup(FS, FileName, NULL);
	if(dentry == NULL) {
		return -ERROR_FILE_NOT_FOUND;
	}
	DokanFileInfo->IsDirectory = (dentry->Attribute & FILE_ATTRIBUTE_DIRECTORY) != 0;
	DokanFileInfo->Context = (ULONG64)dentry;
	return STATUS_SUCCESS;
}

NTSTATUS FS_FAT_GetFileInformation(FILESYSTEM *FS, LPBY_HANDLE_FILE_INFORMATION HandleFileInfo, PDOKAN_FILE_INFO DokanFileInfo)
{
	FAT_DIR_ENTRY *dentry = (FAT_DIR_ENTRY*)DokanFileInfo->Context;
	FAT_FILL_FILE_INFO(HandleFileInfo);
	HandleFileInfo->nNumberOfLinks = 1;
	// TODO: Will have to be changed for FAT32.
	HandleFileInfo->nFileIndexHigh = 0;
	HandleFileInfo->nFileIndexLow = dentry->FirstCluster;
	return STATUS_SUCCESS;
}

NTSTATUS FS_FAT_ReadFile(FILESYSTEM *FS, uint8_t *Buffer, DWORD BufferLength, LPDWORD ReadLength, LONGLONG Offset, PDOKAN_FILE_INFO DokanFileInfo)
{
	FAT_INFO_GET;
	FAT_DIR_ENTRY *dentry = (FAT_DIR_ENTRY*)DokanFileInfo->Context;
	fat_cluster_t cluster = dentry->FirstCluster;
	while(Offset > fat_info->ClusterSize) {
		if(cluster == fat_info->ClusterChainEnd || cluster < 2) {
			return STATUS_DISK_CORRUPT_ERROR;
		}
		cluster = FAT_ClusterLookup(fat_info, cluster);
		Offset -= fat_info->ClusterSize;
	}
	uint8_t *data = FAT_AtCluster(fat_info, cluster);
	if(!data) {
		return STATUS_DISK_CORRUPT_ERROR;
	}
	data += Offset;
	DWORD remaining_in_cluster = fat_info->ClusterSize - (DWORD)Offset;
	while(BufferLength) {
		DWORD copy_length = min(remaining_in_cluster, BufferLength);
		memcpy(Buffer, data, copy_length);
		BufferLength -= copy_length;
		Buffer += copy_length;
		*ReadLength += copy_length;

		if(BufferLength != 0) {
			remaining_in_cluster = fat_info->ClusterSize;
			if(cluster == fat_info->ClusterChainEnd || cluster < 2) {
				return STATUS_DISK_CORRUPT_ERROR;
			}
			cluster = FAT_ClusterLookup(fat_info, cluster);
			data = FAT_AtCluster(fat_info, cluster);
			if(!data) {
				return STATUS_DISK_CORRUPT_ERROR;
			}
		}
	}
	return STATUS_SUCCESS;
}

LONGLONG FS_FAT_FileSize(const void *DEntry)
{
	return ((FAT_DIR_ENTRY*)DEntry)->Size;
}

NEW_FSFORMAT(FAT, 260, W);
