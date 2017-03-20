#include "Playlist.h"

template <class Q>
HRESULT GetEventObject(IMFMediaEvent *pEvent, Q **ppObject)
{
	*ppObject = NULL;   // zero output

	PROPVARIANT var;
	HRESULT hr = pEvent->GetValue(&var);
	if (SUCCEEDED(hr))
	{
		if (var.vt == VT_UNKNOWN)
		{
			hr = var.punkVal->QueryInterface(ppObject);
		}
		else
		{
			hr = MF_E_INVALIDTYPE;
		}
		PropVariantClear(&var);
	}
	return hr;
}

template <class Q>
HRESULT GetCollectionObject(IMFCollection *pCollection, DWORD dwIndex, Q **ppObject)
{
	*ppObject = NULL;   // zero output

	IUnknown *pUnk = NULL;
	HRESULT hr = pCollection->GetElement(dwIndex, &pUnk);
	if (SUCCEEDED(hr))
	{
		hr = pUnk->QueryInterface(IID_PPV_ARGS(ppObject));
		pUnk->Release();
	}
	return hr;
}

HRESULT GetDurationFromTopology(IMFTopology *pTopology, LONGLONG *phnsDuration)
{
	*phnsDuration = 0;

	IMFCollection *pSourceNodes = NULL;
	IMFTopologyNode *pNode = NULL;
	IMFPresentationDescriptor *pPD = NULL;

	HRESULT hr = pTopology->GetSourceNodeCollection(&pSourceNodes);
	if (FAILED(hr))
	{
		goto done;
	}

	hr = GetCollectionObject(pSourceNodes, 0, &pNode);
	if (FAILED(hr))
	{
		goto done;
	}

	hr = pNode->GetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, IID_PPV_ARGS(&pPD));
	if (FAILED(hr))
	{
		goto done;
	}

	*phnsDuration = MFGetAttributeUINT64(pPD, MF_PD_DURATION, 0);

done:
	SafeRelease(&pSourceNodes);
	SafeRelease(&pNode);
	SafeRelease(&pPD);
	return hr;
}

/*static*/
HRESULT CPlaylist::CreateInstance(HWND hwnd, CPlaylist **ppPlayer)
{
	*ppPlayer = NULL;

	HRESULT hr = S_OK;

	CPlaylist *pPlayer = new (std::nothrow) CPlaylist(hwnd, &hr);
	if (pPlayer == NULL)
	{
		return E_OUTOFMEMORY;
	}

	if (SUCCEEDED(hr))
	{
		hr = pPlayer->Initialize();
	}

	if (SUCCEEDED(hr))
	{
		*ppPlayer = pPlayer;
	}
	else
	{
		pPlayer->Release();
	}
	return hr;
}

HRESULT CPlaylist::Initialize()
{
	HRESULT hr = S_OK;
	IMFClock *pClock = NULL;

	hr = CPlayer::Initialize();
	if (FAILED(hr))
	{
		goto done;
	}

	hr = CPlayer::CreateSession();
	if (FAILED(hr))
	{
		goto done;
	}

	// Create an instance of the sequencer Source.
	hr = MFCreateSequencerSource(NULL, &m_pSequencerSource);
	if (FAILED(hr))
	{
		goto done;
	}

	hr = m_pSequencerSource->QueryInterface(IID_PPV_ARGS(&m_pSource));
	if (FAILED(hr))
	{
		goto done;
	}

	// Get the presentation clock.
	hr = m_pSession->GetClock(&pClock);
	if (FAILED(hr))
	{
		goto done;
	}
	hr = pClock->QueryInterface(IID_PPV_ARGS(&m_pPresentationClock));

done:
	SafeRelease(&pClock);
	return hr;
}

CPlaylist::CPlaylist(HWND hWnd, HRESULT *phr)
	: CPlayer(hWnd, hWnd),
	m_pSequencerSource(NULL),
	m_pPresentationClock(NULL),
	m_PresentationTimeOffset(0),
	m_ActiveSegment((DWORD)-1),
	m_hnsSegmentDuration(0),
	m_count(0)
{
}

CPlaylist::~CPlaylist()
{
	SafeRelease(&m_pSequencerSource);
	SafeRelease(&m_pPresentationClock);
}

// Adds a segment to the sequencer.

HRESULT CPlaylist::AddSegment(PCWSTR pszURL)
{
	IMFMediaSource *pMediaSource = NULL;
	IMFPresentationDescriptor *pPD = NULL;
	IMFTopology *pTopology = NULL;

	MFSequencerElementId SegmentId;
	TOPOID TopologyID = 0;

	HRESULT hr = CreateMediaSource(pszURL, &pMediaSource);
	if (FAILED(hr))
	{
		goto done;
	}

	hr = pMediaSource->CreatePresentationDescriptor(&pPD);
	if (FAILED(hr))
	{
		goto done;
	}

	hr = CreatePlaybackTopology(pMediaSource, pPD, m_hwndVideo, &pTopology);
	if (FAILED(hr))
	{
		goto done;
	}

	hr = m_pSequencerSource->AppendTopology(
		pTopology,
		SequencerTopologyFlags_Last,
		&SegmentId
	);

	if (FAILED(hr))
	{
		goto done;
	}

	hr = pTopology->GetTopologyID(&TopologyID);
	if (FAILED(hr))
	{
		goto done;
	}


	if (m_count == 0)
	{
		// Get the segment duration
		m_hnsSegmentDuration = MFGetAttributeUINT64(pPD, MF_PD_DURATION, 0);
	}

	m_segments[m_count].SegmentID = SegmentId;
	m_segments[m_count].TopoID = TopologyID;

	m_count++;

done:
	SafeRelease(&pMediaSource);
	SafeRelease(&pTopology);
	SafeRelease(&pPD);
	return hr;
}

// Adds a new segment to the playlist.

HRESULT CPlaylist::AddToPlaylist(PCWSTR pszURL)
{
	if (NumSegments() >= MAX_PLAYLIST_SEGMENTS)
	{
		return MF_E_OUT_OF_RANGE;
	}

	HRESULT hr = S_OK;

	IMFPresentationDescriptor *pPD = NULL;

	BOOL bFirstSegment = (NumSegments() == 0);

	if (!bFirstSegment)
	{
		// Remove the "last segment" flag from the last segment.
		hr = m_pSequencerSource->UpdateTopologyFlags(LastSegment(), 0);
		if (FAILED(hr))
		{
			goto done;
		}
	}

	// Create the topology and add it to the sequencer.
	hr = AddSegment(pszURL);
	if (FAILED(hr))
	{
		goto done;
	}

	// If this is the first segment, queue it on the session.
	if (bFirstSegment)
	{
		hr = m_pSource->CreatePresentationDescriptor(&pPD);
		m_pFirstPD = pPD;
		m_pFirstPD->AddRef();
		//hr = pPD->Clone(&m_pFirstPD);
		//m_pFirstTopology = 
		if (FAILED(hr))
		{
			goto done;
		}

		hr = QueueNextSegment(pPD);
		if (FAILED(hr))
		{
			goto done;
		}
	}

	if (m_state < Started)
	{
		m_state = OpenPending;
	}

done:
	//SafeRelease(&pPD);
	return hr;
}

// Queues the next topology on the session.

HRESULT CPlaylist::QueueNextSegment(IMFPresentationDescriptor *pPD)
{
	IMFMediaSourceTopologyProvider *pTopoProvider = NULL;
	IMFTopology *pTopology = NULL;

	//Get the topology for the presentation descriptor
	HRESULT hr = m_pSequencerSource->QueryInterface(IID_PPV_ARGS(&pTopoProvider));
	if (FAILED(hr))
	{
		goto done;
	}

	hr = pTopoProvider->GetMediaSourceTopology(pPD, &pTopology);
	if (FAILED(hr))
	{
		goto done;
	}

	//Set the topology on the media session
	m_pSession->SetTopology(NULL, pTopology);

done:
	SafeRelease(&pTopoProvider);
	SafeRelease(&pTopology);
	return hr;
}

// Deletes the corresponding topology from the sequencer source

HRESULT CPlaylist::DeleteSegment(DWORD index)
{
	if (index >= m_count)
	{
		return E_INVALIDARG;
	}

	if (index == m_ActiveSegment)
	{
		return E_INVALIDARG;
	}

	MFSequencerElementId LastSegId = LastSegment();
	MFSequencerElementId SegmentID = m_segments[index].SegmentID;

	HRESULT hr = m_pSequencerSource->DeleteTopology(SegmentID);
	if (FAILED(hr))
	{
		goto done;
	}

	//Delete the segment entry from the list.

	// Move everything up one slot.
	for (DWORD i = index; i < m_count - 1; i++)
	{
		m_segments[i] = m_segments[i + 1];
	}

	m_count--;

	// Is the deleted topology the last one?
	if (LastSegId == SegmentID)
	{
		//Get the new last segment id
		LastSegId = LastSegment();

		//set this topology as the last in the sequencer
		hr = m_pSequencerSource->UpdateTopologyFlags(LastSegId, SequencerTopologyFlags_Last);
	}

done:
	return hr;
}

HRESULT CPlaylist::OnTopologyStatus(IMFMediaEvent *pEvent)
{
	IMFTopology *pTopology = NULL;

	UINT value = 0;
	DWORD  SegmentIndex;
	TOPOID TopoID;

	HRESULT hr = pEvent->GetUINT32(MF_EVENT_TOPOLOGY_STATUS, &value);
	if (FAILED(hr))
	{
		goto done;
	}

	switch (value)
	{
	case MF_TOPOSTATUS_STARTED_SOURCE:

		// Get information about the new segment
		hr = GetEventObject(pEvent, &pTopology);
		if (FAILED(hr))
		{
			hr = S_OK;
			goto done;
		}

		hr = pTopology->GetTopologyID(&TopoID);
		if (FAILED(hr))
		{
			goto done;
		}

		hr = LookupTopoID(TopoID, &SegmentIndex);
		if (FAILED(hr))
		{
			goto done;
		}

		m_ActiveSegment = SegmentIndex;
		hr = GetDurationFromTopology(pTopology, &m_hnsSegmentDuration);
		break;

	case MF_TOPOSTATUS_ENDED:
		m_ActiveSegment = (DWORD)-1;
		break;

	case MF_TOPOSTATUS_READY:
		if (m_state == OpenPending)
		{
			m_state = Stopped;
		}
		break;
	}

done:
	SafeRelease(&pTopology);
	return hr;
}

HRESULT CPlaylist::LookupTopoID(TOPOID id, DWORD *pIndex)
{
	DWORD index;
	for (index = 0; index < m_count; index++)
	{
		if (m_segments[index].TopoID == id)
		{
			break;
		}
	}

	if (index == m_count)
	{
		return MF_E_NOT_FOUND;
	}

	*pIndex = index;
	return S_OK;
}

HRESULT CPlaylist::OnSessionEvent(IMFMediaEvent *pEvent, MediaEventType meType)
{
	if (meType == MESessionNotifyPresentationTime)
	{
		m_PresentationTimeOffset =
			MFGetAttributeUINT64(pEvent, MF_EVENT_PRESENTATION_TIME_OFFSET, 0);
	}
	return S_OK;
}

HRESULT CPlaylist::OnNewPresentation(IMFMediaEvent *pEvent)
{
	IMFPresentationDescriptor *pPD = NULL;

	HRESULT hr = GetEventObject(pEvent, &pPD);

	if (SUCCEEDED(hr))
	{
		// Queue the next segment on the media session
		hr = QueueNextSegment(pPD);
	}

	SafeRelease(&pPD);
	return hr;
}

// Skips to the specified segment in the sequencer source

HRESULT CPlaylist::SkipTo(DWORD index)
{
	if (index >= m_count)
	{
		return E_INVALIDARG;
	}

	MFSequencerElementId ID = m_segments[index].SegmentID;

	PROPVARIANT var;

	HRESULT hr = MFCreateSequencerSegmentOffset(ID, NULL, &var);

	if (SUCCEEDED(hr))
	{
		hr = m_pSession->Start(&MF_TIME_FORMAT_SEGMENT_OFFSET, &var);
		PropVariantClear(&var);
	}
	return hr;
}

HRESULT CPlaylist::GetPlaybackTime(
	MFTIME *phnsPresentation,    // Relative to start of presentation.
	MFTIME *phnsSegment          // Relative to start of segment.
)
{
	*phnsPresentation = 0;
	*phnsSegment = 0;

	HRESULT hr = m_pPresentationClock->GetTime(phnsPresentation);

	if (SUCCEEDED(hr))
	{
		*phnsSegment = (*phnsPresentation) - m_PresentationTimeOffset;
	}
	return hr;
}

HRESULT CPlaylist::OnPresentationEnded(IMFMediaEvent *pEvent)
{
	IMFPresentationDescriptor *pPD = NULL;
	HRESULT hr = m_pSource->CreatePresentationDescriptor(&pPD);
	hr = QueueNextSegment(pPD);
	if (FAILED(hr))
	{
		goto done;
	}

done:
	SafeRelease(&pPD);
	return hr;


//	int index = 0;
//	IMFMediaSourceTopologyProvider *pTopoProvider = NULL;
//	IMFTopology *pTopology = NULL;
//
//	//Get the topology for the presentation descriptor
//	HRESULT hr = m_pSequencerSource->QueryInterface(IID_PPV_ARGS(&pTopoProvider));
//	if (FAILED(hr))
//	{
//		goto done;
//	}
//
//	hr = pTopoProvider->GetMediaSourceTopology(m_pFirstPD, &pTopology);
//	if (FAILED(hr))
//	{
//		goto done;
//	}
//
//	//Set the topology on the media session
//	m_pSession->SetTopology(NULL, pTopology);
//
//	if (index >= m_count)
//	{
//		return E_INVALIDARG;
//	}
//
//	MFSequencerElementId ID = m_segments[index].SegmentID;
//
//	PROPVARIANT var;
//
//	hr = MFCreateSequencerSegmentOffset(ID, NULL, &var);
//
//	if (SUCCEEDED(hr))
//	{
//		hr = m_pSession->Start(&MF_TIME_FORMAT_SEGMENT_OFFSET, &var);
//		PropVariantClear(&var);
//	}
//
//done:
//	SafeRelease(&pTopoProvider);
//	SafeRelease(&pTopology);
//	m_state = Stopped;
//	return hr;
}


