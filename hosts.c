#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "hosts.h"
#include "dnsrelated.h"
#include "dnsgenerator.h"
#include "common.h"
#include "utils.h"
#include "downloader.h"
#include "readline.h"
#include "internalsocket.h"
#include "rwlock.h"

static BOOL			Internet = FALSE;

static int			UpdateInterval;

static time_t		LastUpdate = 0;

static const char 	*File = NULL;

static ThreadHandle	GetHosts_Thread;

static RWLock		HostsLock;

static volatile HostsContainer	*MainDynamicContainer = NULL;

static void DynamicHosts_FreeHostsContainer(HostsContainer *Container)
{
	StringChunk_Free(&(Container -> Ipv4Hosts), FALSE);
	StringChunk_Free(&(Container -> Ipv6Hosts), FALSE);
	StringChunk_Free(&(Container -> CNameHosts), FALSE);
	StringChunk_Free(&(Container -> ExcludedDomains), FALSE);
	StringList_Free(&(Container -> Domains));
	ExtendableBuffer_Free(&(Container -> IPs));
}

static int DynamicHosts_Load(void)
{
	FILE			*fp;
	char			Buffer[320];
	ReadLineStatus	Status;

	int		IPv4Count = 0, IPv6Count = 0, CNameCount = 0, ExcludedCount = 0;

	HostsContainer *TempContainer;

	fp = fopen(File, "r");
	if( fp == NULL )
	{
		return -1;
	}

	TempContainer = (HostsContainer *)SafeMalloc(sizeof(HostsContainer));
	if( TempContainer == NULL )
	{
		return -1;
	}

	if( Hosts_InitContainer(TempContainer) != 0 )
	{
		fclose(fp);

		SafeFree(TempContainer);
		return -1;
	}

	while( TRUE )
	{
		Status = ReadLine(fp, Buffer, sizeof(Buffer));
		if( Status == READ_FAILED_OR_END )
			break;

		switch( Hosts_LoadFromMetaLine(TempContainer, Buffer) )
		{
			case HOSTS_TYPE_AAAA:
				++IPv6Count;
				break;

			case HOSTS_TYPE_A:
				++IPv4Count;
				break;

			case HOSTS_TYPE_CNAME:
				++CNameCount;
				break;

			case HOSTS_TYPE_EXCLUEDE:
				++ExcludedCount;
				break;

			default:
				break;
		}

		if( Status == READ_TRUNCATED )
		{
			ERRORMSG("Hosts is too long : %s\n", Buffer);
			ReadLine_GoToNextLine(fp);
		}
	}

	RWLock_WrLock(HostsLock);
	if( MainDynamicContainer != NULL )
	{
		DynamicHosts_FreeHostsContainer((HostsContainer *)MainDynamicContainer);
		SafeFree((void *)MainDynamicContainer);
	}
	MainDynamicContainer = TempContainer;

	RWLock_UnWLock(HostsLock);

	INFO("Loading Hosts completed, %d IPv4 Hosts, %d IPv6 Hosts, %d CName Redirections, %d items are excluded.\n",
		IPv4Count,
		IPv6Count,
		CNameCount,
		ExcludedCount);

	return 0;
}

static BOOL NeedReload(void)
{
	if( File == NULL )
	{
		return FALSE;
	}

	if( time(NULL) - LastUpdate > UpdateInterval )
	{

#ifdef WIN32

		static FILETIME	LastFileTime = {0, 0};
		WIN32_FIND_DATA	Finddata;
		HANDLE			Handle;

		Handle = FindFirstFile(File, &Finddata);

		if( Handle == INVALID_HANDLE_VALUE )
		{
			return FALSE;
		}

		if( memcmp(&LastFileTime, &(Finddata.ftLastWriteTime), sizeof(FILETIME)) != 0 )
		{
			LastUpdate = time(NULL);
			LastFileTime = Finddata.ftLastWriteTime;
			FindClose(Handle);
			return TRUE;
		} else {
			LastUpdate = time(NULL);
			FindClose(Handle);
			return FALSE;
		}

#else /* WIN32 */
		static time_t	LastFileTime = 0;
		struct stat		FileStat;

		if( stat(File, &FileStat) != 0 )
		{

			return FALSE;
		}

		if( LastFileTime != FileStat.st_mtime )
		{
			LastUpdate = time(NULL);
			LastFileTime = FileStat.st_mtime;

			return TRUE;
		} else {
			LastUpdate = time(NULL);

			return FALSE;
		}

#endif /* WIN32 */
	} else {
		return FALSE;
	}
}

static int TryLoadHosts(void)
{
	if( NeedReload() == TRUE )
	{
		ThreadHandle t = INVALID_THREAD;
		CREATE_THREAD(DynamicHosts_Load, NULL, t);
		DETACH_THREAD(t);
	}
	return 0;
}

static void GetHostsFromInternet_Thread(void *Unused)
{
	const char *URL = ConfigGetRawString(&ConfigInfo, "Hosts");
	const char *Script = ConfigGetRawString(&ConfigInfo, "HostsScript");
	int			HostsRetryInterval = ConfigGetInt32(&ConfigInfo, "HostsRetryInterval");

	INFO("Hosts File : \"%s\" -> \"%s\"\n", URL, File);

	while(1)
	{

		INFO("Getting Hosts From %s ...\n", URL);

		if( GetFromInternet(URL, File) == 0 )
		{
			INFO("Hosts saved at %s.\n", File);

			if( Script != NULL )
			{
				INFO("Running script ...\n");
				system(Script);
			}

			DynamicHosts_Load();

			if( UpdateInterval < 0 )
			{
				return;
			}

			SLEEP(UpdateInterval * 1000);

		} else {
			ERRORMSG("Getting Hosts from Internet failed. Waiting %d second(s) for retry.\n", HostsRetryInterval);
			SLEEP(HostsRetryInterval * 1000);
		}
	}
}

static const char *Hosts_FindFromContainer(HostsContainer *Container, StringChunk *SubContainer, const char *Name)
{
	OffsetOfHosts *IP;

	if( StringChunk_Match(SubContainer, Name, NULL, (char **)&IP) == TRUE )
	{
		return ExtendableBuffer_GetPositionByOffset(&(Container -> IPs), IP -> Offset);
	} else {
		return NULL;
	}
}

static const char *Hosts_FindIPv4(HostsContainer *Container, const char *Name)
{
	return Hosts_FindFromContainer(Container, &(Container -> Ipv4Hosts), Name);
}

static const char *Hosts_FindIPv6(HostsContainer *Container, const char *Name)
{
	return Hosts_FindFromContainer(Container, &(Container -> Ipv6Hosts), Name);
}

static const char *Hosts_FindCName(HostsContainer *Container, const char *Name)
{
	return Hosts_FindFromContainer(Container, &(Container -> CNameHosts), Name);
}

static BOOL Hosts_IsExcludedDomain(HostsContainer *Container, const char *Name)
{
	return StringChunk_Match((StringChunk *)&(Container -> ExcludedDomains), Name, NULL, NULL);
}

#define	MATCH_STATE_PERFECT	0
#define	MATCH_STATE_ONLY_CNAME	1
#define	MATCH_STATE_NONE	(-1)
static int Hosts_Match(HostsContainer *Container, const char *Name, DNSRecordType Type, const char **Result)
{
	if( Container == NULL )
	{
		return MATCH_STATE_NONE;
	}

	if( Hosts_IsExcludedDomain(Container, Name) == TRUE )
	{
		return MATCH_STATE_NONE;
	}

	switch( Type )
	{
		case DNS_TYPE_A:
			*Result = Hosts_FindIPv4(Container, Name);
			if( *Result == NULL )
			{
				break;
			}

			return MATCH_STATE_PERFECT;
			break;

		case DNS_TYPE_AAAA:
			*Result = Hosts_FindIPv6(Container, Name);
			if( *Result == NULL )
			{
				break;
			}

			return MATCH_STATE_PERFECT;
			break;

		case DNS_TYPE_CNAME:
			*Result = Hosts_FindCName(Container, Name);
			if( *Result == NULL )
			{
				return MATCH_STATE_NONE;
			}

			return MATCH_STATE_PERFECT;
			break;

		default:
			return MATCH_STATE_NONE;
			break;
	}

	*Result = Hosts_FindCName(Container, Name);
	if( *Result == NULL )
	{
		return MATCH_STATE_NONE;
	}

	return MATCH_STATE_ONLY_CNAME;
}

static int Hosts_GenerateSingleRecord(DNSRecordType Type, const char *IPOrCName, char *Buffer)
{
	int RecordLength;

	switch( Type )
	{
		case DNS_TYPE_A:
			RecordLength = 2 + 2 + 2 + 4 + 2 + 4;
			break;

		case DNS_TYPE_AAAA:
			RecordLength = 2 + 2 + 2 + 4 + 2 + 16;
			break;

		case DNS_TYPE_CNAME:
			RecordLength = 2 + 2 + 2 + 4 + 2 + strlen(IPOrCName) + 2;
			break;

		default:
			return -1;
			break;
	}

	DNSGenResourceRecord(Buffer + 1, INT_MAX, "", Type, DNS_CLASS_IN, 60, IPOrCName, 4, FALSE);

	Buffer[0] = 0xC0;
	Buffer[1] = 0x0C;

	return RecordLength;
}

static void GetAnswersByName(SOCKET Socket, int Identifier, const char *Name, DNSRecordType Type)
{
	static char	RequestEntity[384] = {
		00, 00, /* QueryIdentifier */
		01, 00, /* Flags */
		00, 01, /* QuestionCount */
		00, 00, /* AnswerCount */
		00, 00, /* NameServerCount */
		00, 00, /* AdditionalCount */
		/* Header end */
	};

	int RequestLength = 12;

	char *NamePos = RequestEntity + 0x0C;

	RequestLength += DNSGenQuestionRecord(NamePos, sizeof(RequestEntity) - 12, Name, Type, DNS_CLASS_IN);
	if( RequestLength == 12 )
	{
        return;
	}

	*(uint16_t *)RequestEntity = Identifier;

	InternalInterface_SendTo(INTERNAL_INTERFACE_UDP_INCOME, Socket, RequestEntity, RequestLength);
}

int DynamicHosts_SocketLoop(void)
{
	static uint16_t	NewIdentifier;

	static QueryContext	Context;

    SOCKET	HostsIncomeSocket;
    SOCKET	HostsOutcomeSocket;
    SOCKET	SendBackSocket;

	static fd_set	ReadSet, ReadySet;

	static const struct timeval	LongTime = {3600, 0};
	static const struct timeval	ShortTime = {2, 0};

	struct timeval	TimeLimit = LongTime;

	int		MaxFd;

	static char		RequestEntity[2048];
	ControlHeader	*Header = (ControlHeader *)RequestEntity;

    HostsIncomeSocket = InternalInterface_TryOpenLocal(10200, INTERNAL_INTERFACE_HOSTS);
    HostsOutcomeSocket = InternalInterface_OpenASocket(MainFamily, NULL);

    SendBackSocket = InternalInterface_GetSocket(INTERNAL_INTERFACE_UDP_INCOME);

	if( HostsOutcomeSocket == INVALID_SOCKET )
	{
		return -1;
	}

	MaxFd = HostsIncomeSocket > HostsOutcomeSocket ? HostsIncomeSocket : HostsOutcomeSocket;
	FD_ZERO(&ReadSet);
	FD_ZERO(&ReadySet);
	FD_SET(HostsIncomeSocket, &ReadSet);
	FD_SET(HostsOutcomeSocket, &ReadSet);

	InternalInterface_InitQueryContext(&Context);

	NewIdentifier = rand();

	while( TRUE )
	{
		ReadySet = ReadSet;

		switch( select(MaxFd + 1, &ReadySet, NULL, NULL, &TimeLimit) )
		{
			case SOCKET_ERROR:
				break;

			case 0:
				if( InternalInterface_QueryContextSwep(&Context, 2, NULL) == TRUE )
				{
					TimeLimit = LongTime;
				} else {
					TimeLimit = ShortTime;
				}
				break;

			default:
				TimeLimit = ShortTime;

				if( FD_ISSET(HostsIncomeSocket, &ReadySet) )
				{
					int State;
					int TotalLength = 0;
					int	MatchState;
					char *MatchResult = NULL;
					BOOL GetLock = FALSE;
					BOOL NeededSendBack = TRUE;

					State = recvfrom(HostsIncomeSocket,
									RequestEntity,
									sizeof(RequestEntity),
									0,
									NULL,
									NULL
									);

					if( State < 1 )
					{
						break;
					}

					if( Internet == FALSE )
					{
						TryLoadHosts();
					}

					MatchState = Hosts_Match(&MainStaticContainer, Header -> RequestingDomain, Header -> RequestingType, &MatchResult);
					if( MatchState == MATCH_STATE_NONE && MainDynamicContainer != NULL )
					{
						RWLock_WrLock(HostsLock);
						MatchState = Hosts_Match(MainDynamicContainer, Header -> RequestingDomain, Header -> RequestingType, &MatchResult);

						GetLock = TRUE;
					}

					switch( MatchState )
					{
						case MATCH_STATE_PERFECT:
							((DNSHeader *)(RequestEntity + sizeof(ControlHeader))) -> Flags.Direction = 1;
							((DNSHeader *)(RequestEntity + sizeof(ControlHeader))) -> Flags.AuthoritativeAnswer = 0;
							((DNSHeader *)(RequestEntity + sizeof(ControlHeader))) -> Flags.RecursionAvailable = 1;
							((DNSHeader *)(RequestEntity + sizeof(ControlHeader))) -> Flags.ResponseCode = 0;
							((DNSHeader *)(RequestEntity + sizeof(ControlHeader))) -> Flags.Type = 0;
							DNSSetAnswerCount(RequestEntity + sizeof(ControlHeader), 1);
							TotalLength = State;
							TotalLength += Hosts_GenerateSingleRecord(Header -> RequestingType, MatchResult, RequestEntity + State);
							NeededSendBack = TRUE;
							break;

						case MATCH_STATE_ONLY_CNAME:
							InternalInterface_QueryContextAddHosts(&Context,
																	Header,
																	NewIdentifier,
																	ELFHash(MatchResult, 0)
																	);

							GetAnswersByName(HostsOutcomeSocket, NewIdentifier, MatchResult, Header -> RequestingType);
							++NewIdentifier;
							NeededSendBack = FALSE;
							break;
					}

					if( GetLock == TRUE )
					{
						RWLock_UnWLock(HostsLock);
					}

					if( NeededSendBack == TRUE )
					{
						if( Header -> NeededHeader == TRUE )
						{
							sendto(SendBackSocket,
									(const char *)Header,
									TotalLength,
									0,
									(const struct sockaddr *)&(Header -> BackAddress.Addr),
									GetAddressLength(Header -> BackAddress.family)
									);
						} else {
							sendto(SendBackSocket,
									RequestEntity + sizeof(ControlHeader),
									TotalLength - sizeof(ControlHeader),
									0,
									(const struct sockaddr *)&(Header -> BackAddress.Addr),
									GetAddressLength(Header -> BackAddress.family)
									);
						}

						ShowNormalMassage(Header -> Agent,
											Header -> RequestingDomain,
											RequestEntity + sizeof(ControlHeader),
											TotalLength - sizeof(ControlHeader),
											'H'
											);
					}


				} else {
					int State;
					static char	NewlyGeneratedRocord[2048];
					ControlHeader	*NewHeader = (ControlHeader *)NewlyGeneratedRocord;

					int NewGeneratedLength = sizeof(ControlHeader);
					int CompressedLength;

					int32_t	EntryNumber;
					QueryContextEntry	*Entry;

					char	*Result = RequestEntity;

					char *AnswersPos;

					State = recvfrom(HostsOutcomeSocket,
									Result,
									sizeof(RequestEntity),
									0,
									NULL,
									NULL
									);

					if( State < 1 )
					{
						break;
					}

					DNSGetHostName(Result,
									DNSJumpHeader(Result),
									NewHeader -> RequestingDomain
									);

					NewHeader -> RequestingDomainHashValue = ELFHash(NewHeader -> RequestingDomain, 0);

					DNSSetNameServerCount(Result, 0);
					DNSSetAdditionalCount(Result, 0);

					AnswersPos = DNSJumpOverQuestionRecords(Result);

					if( DNSExpandCName_MoreSpaceNeeded(Result) > sizeof(RequestEntity) - State )
					{
						break;
					}

					DNSExpandCName(Result);

					EntryNumber = InternalInterface_QueryContextFind(&Context,
																	*(uint16_t *)Result,
																	NewHeader -> RequestingDomainHashValue
																	);

					if( EntryNumber < 0 )
					{
						break;
					}

					Entry = Bst_GetDataByNumber(&Context, EntryNumber);

					memcpy(NewlyGeneratedRocord + NewGeneratedLength, Result, 12);
					*(uint16_t *)(NewlyGeneratedRocord + sizeof(ControlHeader)) = Entry -> Context.Hosts.Identifier;
					NewGeneratedLength += 12;

					NewGeneratedLength += DNSGenQuestionRecord(NewlyGeneratedRocord + NewGeneratedLength,
																sizeof(NewlyGeneratedRocord) - NewGeneratedLength,
																Entry -> Domain,
																Entry -> Type,
																DNS_CLASS_IN
																);

					NewGeneratedLength += DNSGenResourceRecord(NewlyGeneratedRocord + NewGeneratedLength,
																sizeof(NewlyGeneratedRocord) - NewGeneratedLength,
																Entry -> Domain,
																DNS_TYPE_CNAME,
																DNS_CLASS_IN,
                                                                60,
                                                                DNSJumpHeader(Result),
																strlen(DNSJumpHeader(Result)) + 1,
																FALSE
																);

					memcpy(NewlyGeneratedRocord + NewGeneratedLength, AnswersPos, DNSJumpOverAnswerRecords(Result) - AnswersPos);
					NewGeneratedLength += (DNSJumpOverAnswerRecords(Result) - AnswersPos);

					DNSSetNameServerCount(NewlyGeneratedRocord + sizeof(ControlHeader), 0);
					DNSSetAdditionalCount(NewlyGeneratedRocord + sizeof(ControlHeader), 0);
					DNSSetAnswerCount(NewlyGeneratedRocord + sizeof(ControlHeader), DNSGetAnswerCount(NewlyGeneratedRocord + sizeof(ControlHeader)) + 1);

					CompressedLength = DNSCompress(NewlyGeneratedRocord + sizeof(ControlHeader), NewGeneratedLength - sizeof(ControlHeader));

					if( Entry -> NeededHeader == TRUE )
					{
						sendto(SendBackSocket,
								NewlyGeneratedRocord,
								CompressedLength + sizeof(ControlHeader),
								0,
								(const struct sockaddr *)&(Entry -> Context.Hosts.BackAddress.Addr),
								GetAddressLength(Entry -> Context.Hosts.BackAddress.family)
								);
					} else {

						sendto(SendBackSocket,
								NewlyGeneratedRocord + sizeof(ControlHeader),
								CompressedLength,
								0,
								(const struct sockaddr *)&(Entry -> Context.Hosts.BackAddress.Addr),
								GetAddressLength(Entry -> Context.Hosts.BackAddress.family)
								);
					}

					InternalInterface_QueryContextRemoveByNumber(&Context, EntryNumber);

					ShowNormalMassage(Entry -> Agent,
										Entry -> Domain,
										NewlyGeneratedRocord + sizeof(ControlHeader),
										CompressedLength,
										'H'
										);
				}
		}
	}

}

BOOL Hosts_Try(const char *Domain, int Type)
{
	int MatchState;
	char *Result;

	MatchState = Hosts_Match(&MainStaticContainer, Domain, Type, &Result);
	if( MatchState == MATCH_STATE_NONE )
	{
		if( MainDynamicContainer != NULL )
		{
			RWLock_WrLock(HostsLock);
			MatchState = Hosts_Match(MainDynamicContainer, Domain, Type, &Result);
			RWLock_UnWLock(HostsLock);

			if( MatchState == MATCH_STATE_NONE )
			{
				return FALSE;
			} else {
				return TRUE;
			}
		} else {
			return FALSE;
		}
	} else {
		return TRUE;
	}

}

int DynamicHosts_Init(void)
{
	const char	*Path;

	StaticHosts_Init();

	Path = ConfigGetRawString(&ConfigInfo, "Hosts");

	if( Path == NULL )
	{
		File = NULL;
		return 0;
	}

	UpdateInterval = ConfigGetInt32(&ConfigInfo, "HostsUpdateInterval");

	RWLock_Init(HostsLock);

	if( strncmp(Path, "http", 4) != 0 && strncmp(Path, "ftp", 3) != 0 )
	{
		/* Local file */
		File = Path;

		if( DynamicHosts_Load() != 0 )
		{
			ERRORMSG("Loading Hosts failed.\n");
			File = NULL;
			return 1;
		}

	} else {
		/* Internet file */
		File = ConfigGetRawString(&ConfigInfo, "HostsDownloadPath");

		if( ConfigGetInt32(&ConfigInfo, "HostsRetryInterval") < 1 )
		{
			ERRORMSG("`HostsFlushTimeOnFailed' is too small (< 1).\n");
			File = NULL;
			return 1;
		}

		Internet = TRUE;

		if( FileIsReadable(File) )
		{
			INFO("Loading the existing Hosts ...\n");
			DynamicHosts_Load();
		} else {
			INFO("Hosts file is unreadable, this may cause some failures.\n");
		}
	}

	LastUpdate = time(NULL);

	return 0;

}

int DynamicHosts_Start(void)
{
	ThreadHandle	t;
	CREATE_THREAD(DynamicHosts_SocketLoop, NULL, t);
	DETACH_THREAD(t);

	if( Internet == TRUE )
	{
		CREATE_THREAD(GetHostsFromInternet_Thread, NULL, GetHosts_Thread);
	}
}