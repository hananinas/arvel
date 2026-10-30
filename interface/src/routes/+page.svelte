<script lang="ts">
	import { onMount, tick } from 'svelte';
	import NativeStatePreview from '$lib/components/NativeStatePreview.svelte';
    import { DeviceRequestError, deviceRequest, isAgentSnapshot, safeSourceUrl, fetchHistory, fetchHistoryPage,
        fetchHistoryExportPage, memoryAction, historyExportKinds, fetchDesk, type AgentSnapshot, type AgentTool,
        type AgentSource, type ConversationPage, type ConversationSummary, type HistoryPage, type HistoryRowsPage,
        type HistoryExportKind, type DeskSnapshot, type DeskNote } from '$lib/agent';
	import { toolIconMarkup, toolKind } from '$lib/arvelToolIcons';
	import { checkOwnerAccess, ownerAccess, requireOwnerLogin } from '$lib/ownerAccess';

	let snapshot = $state<AgentSnapshot | null>(null);
	let connected = $state(false);
	let loading = $state(true);
	let draft = $state('');
	let pending = $state<'send' | 'reset' | 'control' | null>(null);
	let requestError = $state('');
	let connectionError = $state('');
	let notice = $state('');
	let awaitingDevice = $state(false);
	let acceptedRevision = -1;
	let historyElement: HTMLDivElement;
	let composerElement: HTMLTextAreaElement;
	let mounted = false;
	let pollTimer: ReturnType<typeof setTimeout>;
	let polling = $state(false);
	const requests = new Set<AbortController>();
    let archive = $state<ConversationPage | null>(null);
    let archivedMessages = $state<HistoryPage | null>(null);
    let archivedEvidence = $state<HistoryRowsPage | null>(null);
    let selectedConversation = $state<ConversationSummary | null>(null);
    let historyBusy = $state(false);
    let historyError = $state('');
    let historyNotice = $state('');
    let historyLiteral = $state('');
    let historyKind = $state<HistoryExportKind>('messages');
    let listCursor = $state(0);
    let messageCursor = $state(0);
    let listBack = $state<number[]>([]);
    let messageBack = $state<number[]>([]);
    let exportProgress = $state(0);
    let exportFinished = $state(false);
    let memoryController: AbortController | null = null;
    let stopping = $state(false);
    let desk = $state<DeskSnapshot | null>(null);
    let deskAt = 0;
    let deskLoading = false;
    let deskError = $state('');
    let archiveRequested = false;
    let now = $state(Date.now());
    let showRecords = $state(false);
	const draftKey = 'unravel-buddy-draft';
	const textEncoder = new TextEncoder();
	let draftBytes = $derived(textEncoder.encode(draft).length);
	let ownerReady = $derived($ownerAccess.status?.authenticated === true);
	let loginNeeded = $derived($ownerAccess.status?.authenticated === false);
	let canControl = $derived(ownerReady && connected && !!snapshot && !pending);
	let canSend = $derived(canControl && !!snapshot && snapshot.wifiConnected && snapshot.chatConfigured &&
		!snapshot.busy && !awaitingDevice && !snapshot.voiceActive && !snapshot.voiceConnecting &&
		snapshot.conversationMode === 'text_pipeline' &&
		draft.trim().length > 0 && draftBytes <= 8192);
	let buddyName = $derived(snapshot?.buddyName || 'Arvel');
	let micSession = $derived(snapshot ? snapshot.voiceActive || snapshot.voiceConnecting || snapshot.state === 'recording' : false);
	let voiceToggleLabel = $derived(snapshot?.voiceConnecting ? 'Voice connecting…' :
		snapshot?.voiceLiveAvailable && snapshot.voiceActive ? snapshot.voiceMuted ? 'Unmute device mic' : 'Mute device mic' :
		snapshot?.state === 'recording' ? 'Stop recording & send' : 'Start device voice');
	let voiceToggleDisabled = $derived(!canControl || !snapshot?.voiceConfigured || snapshot.voiceConnecting ||
		(snapshot.busy && snapshot.state !== 'recording' && !snapshot.voiceActive));

	function saveDraft() {
		try { sessionStorage.setItem(draftKey, draft); } catch { /* A draft still stays in memory when storage is unavailable. */ }
	}

	function accessExpired() {
		requireOwnerLogin();
		snapshot = null;
        memoryController?.abort();
        archive = null; archivedMessages = null; archivedEvidence = null; selectedConversation = null;
        exportProgress = 0; exportFinished = false; historyNotice = ''; historyError = '';
        desk = null; deskAt = 0; archiveRequested = false;
		connected = false;
		awaitingDevice = false;
		notice = '';
	}

	function requestFailure(error: unknown) {
		if (error instanceof DeviceRequestError && error.status === 401) accessExpired();
		return error instanceof Error ? error.message : 'Cannot reach Arvel. Check the device connection and retry.';
	}

	async function refresh() {
		if (!mounted || polling) return;
		clearTimeout(pollTimer);
		if (document.hidden) { pollTimer = setTimeout(refresh, 10000); return; }
		polling = true;
		const controller = new AbortController();
		requests.add(controller);
		try {
			if (!ownerReady) {
				const access = await checkOwnerAccess();
				if (!mounted) return;
				if (!access.authenticated) {
					snapshot = null;
					connected = false;
					connectionError = '';
					return;
				}
			}
			const data = await deviceRequest<unknown>('/rest/device', { signal: controller.signal }, 5000);
            if (!mounted || !ownerReady) return;
			if (!isAgentSnapshot(data)) throw new Error('Device status is unreadable. Check that Arvel’s firmware supports the native state preview, then retry.');
			const atBottom = !historyElement || historyElement.scrollHeight - historyElement.scrollTop - historyElement.clientHeight < 80;
			const changed = snapshot?.messageRevision !== data.messageRevision;
			snapshot = data;
			connected = true;
			connectionError = '';
			if (awaitingDevice && (data.busy || data.messageRevision !== acceptedRevision || data.state === 'error')) {
				awaitingDevice = false;
				notice = data.busy ? 'Arvel is working on it.' : '';
			}
			if (!awaitingDevice && !data.busy) notice = '';
			// Tools may have changed notes or the timer, so a new message refreshes the desk early.
			if (changed || Date.now() - deskAt > 10000) void loadDesk();
			if (!archiveRequested) { archiveRequested = true; void loadArchive(); }
			if (changed && atBottom) {
				await tick();
				if (mounted && historyElement) historyElement.scrollTop = historyElement.scrollHeight;
			}
		} catch (error) {
			if (!mounted) return;
			connected = false;
			snapshot = null;
			connectionError = requestFailure(error);
		} finally {
			requests.delete(controller);
			polling = false;
			if (mounted) {
				loading = false;
				pollTimer = setTimeout(refresh, !ownerReady ? 8000 : snapshot?.busy || snapshot?.voiceActive || snapshot?.voiceConnecting || awaitingDevice ? 1000 : 4000);
			}
		}
	}

	async function sendMessage(event?: SubmitEvent) {
		event?.preventDefault();
		if (!canSend) return;
		const submittedDraft = draft;
		acceptedRevision = snapshot?.messageRevision ?? -1;
		pending = 'send';
		requestError = '';
		notice = '';
		const controller = new AbortController();
		requests.add(controller);
		try {
			await deviceRequest('/rest/device/chat', {
				method: 'POST', signal: controller.signal,
				body: JSON.stringify({ text: submittedDraft, speak: false })
			});
            if (!mounted || !ownerReady) return;
			awaitingDevice = true;
			if (draft === submittedDraft) { draft = ''; saveDraft(); }
			notice = 'Sent to Arvel’s native conversation. Waiting for the device.';
			await refresh();
		} catch (error) {
			if (mounted) requestError = requestFailure(error);
		} finally {
			requests.delete(controller);
			if (mounted) { pending = null; composerElement?.focus(); }
		}
	}

    async function resetConversation() {
        if (!canControl || !snapshot) return;
        if (!window.confirm('Start a new conversation? The current conversation will close, but its SD archive is kept. Your unsent draft stays. Voice will end; uncertain actions are not replayed.')) return;
        await changeMemory('new');
    }

	async function remoteCommand(body: object) {
        const stop = 'action' in body && body.action === 'voice_stop';
        if (!(ownerReady && connected && snapshot) || (stop ? stopping : !!pending)) return;
        if (stop) stopping = true;
        else pending = 'control';
		requestError = '';
		const controller = new AbortController();
		requests.add(controller);
		try {
			await deviceRequest('/rest/device/control', { method: 'POST', signal: controller.signal, body: JSON.stringify(body) });
            if (mounted && ownerReady) { notice = 'Command accepted by Arvel.'; await refresh(); }
		} catch (error) {
			if (mounted) requestError = requestFailure(error);
		} finally {
			requests.delete(controller);
            if (mounted) { if (stop) stopping = false; else pending = null; }
		}
	}

	function control(action: 'voice_toggle' | 'voice_stop' | 'screen_toggle' | 'brightness_cycle') {
        if (action === 'voice_stop' ? !(ownerReady && connected && snapshot) : !canControl) return;
		if (action === 'voice_toggle' && !micSession &&
			!window.confirm('Start voice on physical Arvel? Its microphone—not your browser microphone—will capture nearby audio and send it to your configured voice service. Only continue with permission from people nearby.')) return;
		void remoteCommand({ action });
	}


    async function loadDesk() {
        if (!ownerReady || deskLoading) return;
        deskLoading = true;
        const controller = new AbortController();
        requests.add(controller);
        try {
            const value = await fetchDesk(controller.signal);
            if (mounted && ownerReady) { desk = value; deskAt = Date.now(); now = deskAt; deskError = ''; }
        } catch (error) { if (mounted) deskError = requestFailure(error); }
        finally { requests.delete(controller); deskLoading = false; }
    }

    async function loadArchive(after = 0) {
        if (!ownerReady || historyBusy) return;
        historyBusy = true; historyError = '';
        const controller = new AbortController(); memoryController = controller;
        try {
            const page = await fetchHistory({after,signal:controller.signal});
            if (mounted && ownerReady) { archive = page; listCursor = after; }
        } catch (error) { if (mounted) historyError = requestFailure(error); }
        finally { if (memoryController === controller) memoryController = null; if (mounted) historyBusy = false; }
    }
    async function inspectConversation(conversation: ConversationSummary, after = 0, reset = false) {
        if (!ownerReady || historyBusy) return;
        historyBusy = true; historyError = '';
        const controller = new AbortController(); memoryController = controller;
        try {
            const query = {after,count:20,signal:controller.signal};
            const page = historyKind === 'messages'
                ? await fetchHistoryPage(conversation.uuid,{...query,literal:historyLiteral})
                : await fetchHistoryExportPage(conversation.uuid,historyKind,query);
            if (!mounted || !ownerReady) return;
            if (reset || selectedConversation?.uuid !== conversation.uuid) {
                messageBack = []; exportProgress = 0; exportFinished = false;
            }
            selectedConversation = conversation; messageCursor = after;
            archivedMessages = 'messages' in page ? page : null;
            archivedEvidence = 'rows' in page ? page : null;
        } catch (error) { if (mounted) historyError = requestFailure(error); }
        finally { if (memoryController === controller) memoryController = null; if (mounted) historyBusy = false; }
    }
    async function changeMemory(action: 'new' | 'resume' | 'close' | 'delete', uuid?: string) {
        if (!ownerReady || historyBusy) return;
        if (action === 'delete' && !window.confirm('Permanently remove this conversation from the local archive, including its messages, sources and tool evidence? This is a logical deletion, not secure erasure of SD flash. Export first if you need a copy.')) return;
        if (action === 'resume' && !window.confirm('Resume this conversation using bounded recent context? Current voice ends and a fresh provider session is used. Earlier history remains searchable; no previous physical action is replayed.')) return;
        if (action === 'close' && !window.confirm('Close this conversation and end voice? Its local archive stays available.')) return;
        historyBusy = true; historyError = ''; historyNotice = '';
        const controller = new AbortController(); memoryController = controller;
        try {
            await memoryAction(action,uuid,controller.signal);
            if (!mounted || !ownerReady) return;
            historyNotice = 'Memory action accepted. Check the device’s conversation and storage status for the observed result.';
            if (action === 'delete') { selectedConversation = null; archivedMessages = null; archivedEvidence = null; archive = null; }
            await refresh();
        } catch (error) { if (mounted) historyError = requestFailure(error); }
        finally { if (memoryController === controller) memoryController = null; if (mounted) historyBusy = false; }
    }
    async function exportConversation() {
        if (!ownerReady || !selectedConversation || historyBusy) return;
        historyBusy = true; historyError = ''; historyNotice = ''; exportFinished = false; exportProgress = 0;
        const conversation = selectedConversation;
        const controller = new AbortController(); memoryController = controller;
        let kindIndex = 0, cursor = 0, metadata = true;
        // Only one bounded page is fetched per pull. The browser owns the output
        // Blob; firmware never allocates or transmits an archive-sized packet.
        const stream = new ReadableStream<Uint8Array>({
            async pull(output) {
                try {
                    if (controller.signal.aborted || !mounted || !ownerReady)
                        throw new DOMException('Export cancelled.', 'AbortError');
                    if (metadata) {
                        metadata = false;
                        output.enqueue(textEncoder.encode(JSON.stringify({version:1,conversation}) + '\n'));
                        return;
                    }
                    if (kindIndex === historyExportKinds.length) { output.close(); return; }
                    const kind = historyExportKinds[kindIndex];
                    const page = await fetchHistoryExportPage(conversation.uuid,kind,{after:cursor,signal:controller.signal});
                    if (controller.signal.aborted || !mounted || !ownerReady)
                        throw new DOMException('Export cancelled.', 'AbortError');
                    if (page.has_more && page.next_cursor <= cursor) throw new Error('Export cursor did not advance.');
                    output.enqueue(textEncoder.encode(JSON.stringify({kind,page}) + '\n'));
                    if (page.has_more) cursor = page.next_cursor;
                    else { cursor = 0; kindIndex++; }
                    exportProgress++;
                } catch (error) { output.error(error); }
            },
            cancel() { controller.abort(); }
        });
        try {
            const blob = await new Response(stream).blob();
            if (!mounted || !ownerReady || controller.signal.aborted) return;
            const url = URL.createObjectURL(blob), link = document.createElement('a');
            link.href = url; link.download = `arvel-${conversation.uuid}.jsonl`;
            document.body.append(link); link.click(); link.remove();
            setTimeout(() => URL.revokeObjectURL(url),10000);
            exportFinished = true;
            historyNotice = 'All five record kinds streamed into one download. Check your browser saved it. Active conversations can change during export; close one first for a stable archive.';
        } catch (error) { if (mounted) historyError = requestFailure(error); }
        finally { if (memoryController === controller) memoryController = null; if (mounted) historyBusy = false; }
    }

    $effect(() => {
        if (!ownerReady) {
            snapshot = null; connected = false; awaitingDevice = false; notice = '';
            desk = null; deskAt = 0; deskError = ''; archiveRequested = false;
            for (const controller of requests) controller.abort();
            memoryController?.abort();
            archive = null; selectedConversation = null; archivedMessages = null; archivedEvidence = null;
            historyNotice = ''; historyError = ''; exportProgress = 0; exportFinished = false;
        }
    });
    // Count a running timer down locally between device polls.
    $effect(() => {
        if (desk?.timer?.status !== 'running') return;
        const tick = setInterval(() => { now = Date.now(); }, 1000);
        return () => clearInterval(tick);
    });
    let timerRemaining = $derived(!desk?.timer ? 0 : desk.timer.status === 'running'
        ? Math.max(0, desk.timer.remainingSeconds - Math.floor((now - deskAt) / 1000)) : desk.timer.remainingSeconds);
    let timerProgress = $derived(desk?.timer?.durationSeconds ? 1 - timerRemaining / desk.timer.durationSeconds : 0);
    let tasks = $derived((desk?.notes ?? []).filter((note) => note.kind === 'task')
        .sort((a, b) => Number(a.completed) - Number(b.completed) || (a.dueDate || '9999').localeCompare(b.dueDate || '9999') || a.id - b.id));
    let notes = $derived((desk?.notes ?? []).filter((note) => note.kind === 'note').sort((a, b) => b.id - a.id));
    let openTasks = $derived(tasks.filter((task) => !task.completed).length);

    const clock = (seconds: number) => {
        const h = Math.floor(seconds / 3600), m = Math.floor(seconds / 60) % 60, s = seconds % 60;
        return `${h ? `${h}:` : ''}${h ? String(m).padStart(2, '0') : m}:${String(s).padStart(2, '0')}`;
    };
    const localDay = (offset: number) => {
        const d = new Date(); d.setDate(d.getDate() + offset);
        return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
    };
    function due(note: DeskNote): { label: string; overdue: boolean } | null {
        if (!note.dueDate) return null;
        const today = localDay(0);
        if (note.dueDate === today) return { label: 'Today', overdue: false };
        if (note.dueDate === localDay(1)) return { label: 'Tomorrow', overdue: false };
        const [y, m, d] = note.dueDate.split('-').map(Number);
        const label = new Date(y, m - 1, d).toLocaleDateString(undefined, { month: 'short', day: 'numeric' });
        return { label, overdue: !note.completed && note.dueDate < today };
    }
    // Forecast times are UTC seconds; the station offset gives the location's own clock.
    const stationHour = (time: number, offset: number) =>
        new Date((time + offset) * 1000).toLocaleTimeString(undefined, { hour: 'numeric', timeZone: 'UTC' });
    function ago(seconds: number) {
        const ms = seconds > 1e12 ? seconds : seconds * 1000, minutes = Math.round((now - ms) / 60000);
        if (minutes < 1) return 'just now';
        if (minutes < 60) return `${minutes} min ago`;
        const hours = Math.round(minutes / 60);
        return hours < 24 ? `${hours} h ago` : new Date(ms).toLocaleDateString();
    }
    const when = (ms: number | null) => ms ? new Date(ms).toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' }) : 'Date unavailable';
    const stateLabel: Record<string, string> = { running: 'Running', succeeded: 'Done', failed: 'Failed', unknown: 'Uncertain', cancelled: 'Cancelled', partial: 'Partial' };
    function sourceHost(url: string | null) {
        try { return url ? new URL(url).hostname.replace(/^www\./, '') : ''; } catch { return ''; }
    }
	const memory = (bytes: number | undefined) => bytes === undefined ? '—' : `${(bytes / 1024).toLocaleString(undefined, { maximumFractionDigits: 1 })} KiB`;

	onMount(() => {
		mounted = true;
		try { draft = sessionStorage.getItem(draftKey) || ''; } catch { /* Storage is optional. */ }
		const resume = () => { if (!document.hidden) void refresh(); };
		document.addEventListener('visibilitychange', resume);
		window.addEventListener('focus', resume);
		void refresh();
		return () => {
			mounted = false;
			clearTimeout(pollTimer);
            memoryController?.abort();
			document.removeEventListener('visibilitychange', resume);
			window.removeEventListener('focus', resume);
			for (const controller of requests) controller.abort();
			requests.clear();
		};
	});
</script>


<svelte:head>
	<title>Arvel · Home</title>
</svelte:head>

<div class="home">
	<section class="device" aria-label="Arvel’s native state preview">
		<NativeStatePreview snapshot={ownerReady && connected ? snapshot : null} authenticated={ownerReady} {loading} />
		<div class="remote" aria-label="Physical device controls" role="group">
			<button type="button" class="a-btn primary mic-btn" disabled={voiceToggleDisabled} onclick={() => control('voice_toggle')}>
				<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3Z" /><path d="M19 10v2a7 7 0 0 1-14 0v-2" /><line x1="12" x2="12" y1="19" y2="22" /></svg>
				{voiceToggleLabel}
			</button>
			<button type="button" class="a-btn ghost" disabled={!ownerReady || !connected || !snapshot || stopping} onclick={() => control('voice_stop')}>{stopping ? 'Stopping…' : snapshot?.voiceActive ? 'End voice' : snapshot?.voiceConnecting ? 'Cancel connection' : 'Stop'}</button>
			<span class="sep" aria-hidden="true"></span>
			<button type="button" class="a-btn ghost" disabled={!canControl} onclick={() => control('screen_toggle')}>{snapshot?.screenOff ? 'Screen on' : 'Screen off'}</button>
			<button type="button" class="a-btn ghost" disabled={!canControl} onclick={() => control('brightness_cycle')}>Brightness{snapshot ? ` · ${snapshot.preferences.brightness}%` : ''}</button>
			<button type="button" class="a-btn ghost icon-only" disabled={loading || polling} onclick={() => void refresh()} aria-label="Refresh access and status" title="Refresh">
				<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M21 12a9 9 0 1 1-2.64-6.36" /><path d="M21 3v6h-6" /></svg>
			</button>
		</div>
		<p class="device-caption">Drawn live from Arvel’s state at the screen’s real 640 × 172 layout. Voice uses <strong>Arvel’s own microphone</strong>, never your browser’s. <a href="/settings#speech-output">Voice settings</a></p>
		{#if loginNeeded}
			<div class="owner-prompt notice">
				<strong>Owner login required.</strong> Sign in to view the device state, control Arvel and use its native conversation.
				<a class="a-btn primary" href="/login?returnTo=home">Log in to Arvel</a>
				{#if !$ownerAccess.status?.configured}<p>First time? Approve creating your owner login on physical Arvel through Settings → Device → Web login.</p>{/if}
			</div>
		{/if}
		{#if snapshot && !snapshot.voiceConfigured}<p class="device-caption">Voice is not configured. <a href="/settings#speech-output">Set up your voice service</a> before starting the physical microphone.</p>{/if}
	</section>

	<section class="panel-card conversation" aria-labelledby="conversation-heading">
		<header class="card-head">
			<span class="avatar" aria-hidden="true">{buddyName.slice(0, 1)}</span>
			<div class="head-text">
				<h2 id="conversation-heading" tabindex="-1">{buddyName}</h2>
				<p class="card-sub"><span class="dot" class:busy={snapshot?.busy} class:off={!connected}></span>{!connected ? (loading ? 'Connecting…' : 'Offline') : snapshot?.busy ? (snapshot.status || 'Working…') : 'Ready'}{snapshot?.model && snapshot.conversationMode !== 'codex_live' ? ` · ${snapshot.model}` : ''}</p>
			</div>
            <button class="a-btn ghost" onclick={resetConversation} disabled={!canControl || !snapshot || historyBusy}>
				<svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" aria-hidden="true"><path d="M12 5v14M5 12h14" /></svg>
				{historyBusy ? 'Updating…' : 'New chat'}
			</button>
		</header>
		{#if connectionError}<div class="notice error" role="alert"><strong>{loginNeeded ? 'Sign in again.' : 'Connection needs attention.'}</strong> {connectionError} {#if loginNeeded}<a href="/login?returnTo=home">Log in to Arvel</a>{:else}<button type="button" class="inline-retry" onclick={() => void refresh()}>Retry connection</button> Check that Arvel is on your network and that the localhost proxy points to its address.{/if}</div>{/if}
		{#if requestError}<div class="notice error" role="alert"><strong>Request needs attention.</strong> {requestError}</div>{/if}
		{#if snapshot?.lastError}<div class="notice error" role="alert"><strong>Arvel reported an error.</strong> {snapshot.lastError} {#if snapshot.voicePhase === 'error'}Stop device voice above, <a href="/settings#speech-output">check the voice service settings</a>, then start device voice again when ready.{:else}<a href="/settings#chat-provider">Check the configured service settings</a> and the device’s connection before trying again.{/if}</div>{/if}
		{#if snapshot && !snapshot.chatConfigured && snapshot.conversationMode === 'text_pipeline'}<div class="notice">The saved text profile needs an API key. <a href="/settings#chat-provider">Open Settings</a> to add it — never paste keys into the conversation.</div>{/if}
		{#if snapshot && !snapshot.chatConfigured && snapshot.conversationMode === 'codex_live'}<div class="notice"><strong>Codex device tools need a chat backend.</strong> <a href="/settings#chat-provider">Configure the saved text profile</a> to use device actions and search through Codex delegations.</div>{/if}
		<div id="conversation-history" class="history" bind:this={historyElement} role="log" aria-label="Recent conversation" aria-live="polite" aria-relevant="additions text" tabindex="-1">
			{#if !snapshot}
				<div class="empty"><h3>{loading ? 'Connecting to Arvel…' : loginNeeded ? 'Your device, owner access.' : 'Arvel is out of reach.'}</h3><p>{loading ? 'Checking owner access, then reading the native device conversation.' : loginNeeded ? 'Log in to view the device state and use the native conversation. Your draft stays in this tab.' : 'Check the device connection or localhost proxy, then refresh. Your draft stays in this tab.'}</p>{#if loginNeeded}<a class="a-btn primary" href="/login?returnTo=home">Log in to Arvel</a>{/if}</div>
			{:else if snapshot.messages.length === 0}
				<div class="empty"><h3>Say hello to {buddyName}.</h3><p>Ask for a timer, a note, the weather or anything else. Typed messages are answered silently; use the mic button above to talk to the device.</p></div>
			{:else}
				{#each snapshot.messages as message (message.id)}
					{@render bubble(message.role, message.text, message.time, message.interrupted ?? false, message.tools ?? [], message.sources ?? [])}
				{/each}
			{/if}
            {#if snapshot && (snapshot.tools.length || snapshot.sources.length)}
                <section class="live-tools" aria-label="Current tool progress" aria-live="polite">
                    {@render toolList(snapshot.tools)}
                    {@render sourceCards(snapshot.sources)}
                </section>
            {/if}
			{#if snapshot?.busy}<div class="typing" aria-hidden="true"><span></span><span></span><span></span></div>{/if}
		</div>
		<form class="composer" onsubmit={sendMessage}>
			<label for="buddy-message" class="sr-only">Message {buddyName}</label>
			<div class="composer-box" class:over={draftBytes > 8192}>
				<textarea id="buddy-message" bind:this={composerElement} bind:value={draft} rows="2" placeholder={`Message ${buddyName}…`} aria-describedby="composer-help" aria-invalid={draftBytes > 8192} oninput={(event) => { draft = event.currentTarget.value; saveDraft(); }} onkeydown={(event) => { if (event.ctrlKey && event.key === 'Enter' && !event.isComposing) { event.preventDefault(); void sendMessage(); } }}></textarea>
				<button type="submit" class="send" disabled={!canSend} aria-label={pending === 'send' ? 'Sending' : 'Send'}>
					{#if pending === 'send'}<span class="spinner" aria-hidden="true"></span>{:else}<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 19V5M5 12l7-7 7 7" /></svg>{/if}
				</button>
			</div>
			<p id="composer-help" class="hint composer-help"><span>{draftBytes > 8192 ? 'Too long — shorten it to 8,192 UTF-8 bytes.' : 'Ctrl+Enter to send · draft is kept in this tab'}</span>{#if draftBytes > 4096}<span class:over={draftBytes > 8192}>{draftBytes.toLocaleString()} / 8,192 bytes</span>{/if}</p>
			{#if snapshot && snapshot.conversationMode !== 'text_pipeline'}<p class="hint">This live integration accepts speech, not typed messages. <a href="/settings#conversation-model">Switch to your saved text profile</a> to type. No request is silently routed to another model.</p>{/if}
			{#if snapshot?.voiceActive || snapshot?.voiceConnecting}<p class="hint">Stop device voice before sending text or starting a new chat.</p>{/if}
			<p class="hint state-label" role="status" aria-live="polite">{notice}</p>
		</form>
	</section>

	<aside class="side" aria-label="Arvel’s desk">
		{#if desk?.timer}
			{@const t = desk.timer}
			<section class="panel-card desk-card timer-card" aria-labelledby="timer-heading">
				<div class="desk-head">
					<svg class="desk-icon" width="28" height="28" viewBox="0 0 32 32" aria-hidden="true">{@html toolIconMarkup.timer}</svg>
					<h2 id="timer-heading">{t.label || (t.pomodoro ? (t.phase === 'break' ? 'Break' : 'Focus') : 'Timer')}</h2>
					<span class="pill" class:done={t.status === 'done'} class:paused={t.status === 'paused'}>{t.status === 'done' ? 'Finished' : t.status === 'paused' ? 'Paused' : 'Running'}</span>
				</div>
				<p class="timer-clock" role="timer" aria-live="off">{clock(timerRemaining)}</p>
				<div class="progress" role="progressbar" aria-label="Timer progress" aria-valuemin="0" aria-valuemax="100" aria-valuenow={Math.round(timerProgress * 100)}><span style:width={`${Math.min(100, timerProgress * 100)}%`}></span></div>
				<p class="desk-meta">{clock(t.durationSeconds)} total{t.pomodoro && t.rounds ? ` · round ${t.round} of ${t.rounds}` : ''}</p>
			</section>
		{/if}

		{#if desk?.weather}
			{@const w = desk.weather}
			<section class="panel-card desk-card weather-card" aria-labelledby="weather-heading">
				<div class="desk-head">
					<svg class="desk-icon" width="28" height="28" viewBox="0 0 32 32" aria-hidden="true">{@html toolIconMarkup.weather}</svg>
					<h2 id="weather-heading">{w.location || 'Weather'}</h2>
					<span class="desk-meta">{ago(w.fetchedAt)}</span>
				</div>
				<div class="weather-now">
					<p class="temp">{Math.round(w.temperatureC)}°</p>
					<div><p class="condition">{w.condition}</p><p class="desk-meta">Feels {Math.round(w.feelsLikeC)}° · H {Math.round(w.highC)}° L {Math.round(w.lowC)}° · {Math.round(w.windKmh)} km/h</p></div>
				</div>
				{#if w.forecast.length}
					<ol class="forecast" aria-label="Next hours">
						{#each w.forecast as hour (hour.time)}
							<li><span class="desk-meta">{stationHour(hour.time, w.utcOffsetSeconds)}</span><strong>{Math.round(hour.temperatureC)}°</strong><span class="rain" class:wet={hour.precipitationProbability >= 40}>{hour.precipitationProbability}%</span></li>
						{/each}
					</ol>
				{/if}
			</section>
		{/if}

		<section class="panel-card desk-card notes-card" aria-labelledby="notes-heading">
			<div class="desk-head">
				<svg class="desk-icon" width="28" height="28" viewBox="0 0 32 32" aria-hidden="true">{@html toolIconMarkup.notes}</svg>
				<h2 id="notes-heading">Notes & tasks</h2>
				{#if tasks.length}<span class="pill">{openTasks} open</span>{/if}
			</div>
			{#if deskError && !desk}
				<p class="desk-empty">Couldn’t read Arvel’s notes. {deskError}</p>
			{:else if !desk}
				<p class="desk-empty">{ownerReady ? 'Loading…' : 'Log in to see what Arvel is keeping for you.'}</p>
			{:else if !desk.notesReady}
				<p class="desk-empty">Notes storage isn’t ready on the device yet.</p>
			{:else if !desk.notes.length}
				<p class="desk-empty">Nothing saved yet. Try “remind me to water the plants on Friday” or “note that the Wi-Fi password is on the fridge”.</p>
			{:else}
				{#if tasks.length}
					<ul class="task-list" aria-label="Tasks">
						{#each tasks as task (task.id)}
							{@const d = due(task)}
							<li class:completed={task.completed}>
								<span class="check" aria-hidden="true">{#if task.completed}<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3.2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12.5l4.5 4.5L19 7.5" /></svg>{/if}</span>
								<span class="task-text"><span class="sr-only">{task.completed ? 'Done: ' : 'Open: '}</span>{task.text}</span>
								{#if d}<span class="due" class:overdue={d.overdue}>{d.overdue ? 'Overdue · ' : ''}{d.label}</span>{/if}
							</li>
						{/each}
					</ul>
				{/if}
				{#if notes.length}
					<ul class="note-list" aria-label="Notes">
						{#each notes as note (note.id)}
							{@const d = due(note)}
							<li><p>{note.text}</p>{#if d}<span class="due" class:overdue={d.overdue}>{d.label}</span>{/if}</li>
						{/each}
					</ul>
				{/if}
			{/if}
			<p class="desk-foot">Ask {buddyName} to add, edit or complete items. Saved on the device ({desk?.notes.length ?? 0} / 24).</p>
		</section>

		<details class="panel-card facts">
			<summary><h2 id="device-heading">Device details</h2><span class="desk-meta">{snapshot ? (snapshot.wifiConnected ? snapshot.ssid || 'Wi-Fi' : 'Offline') : '—'}</span></summary>
			<h3 class="fact-group">Connection</h3>
			<dl>
				<div><dt>Status</dt><dd>{loginNeeded ? 'Login required' : !connected ? (loading ? 'Connecting' : 'Unreachable') : snapshot?.status || 'Connected'}</dd></div>
				<div><dt>Wi-Fi</dt><dd>{snapshot ? (snapshot.wifiConnected ? snapshot.ssid || 'Connected' : 'Offline') : '—'}</dd></div>
				<div><dt>Address</dt><dd>{snapshot?.ip || '—'}</dd></div>
				<div><dt>Clock</dt><dd>{snapshot ? (snapshot.clockReady ? snapshot.localTime || 'Synced' : 'Syncing') : '—'}</dd></div>
				<div><dt>Screen</dt><dd>{snapshot?.screenOff === undefined ? '—' : snapshot.screenOff ? 'Off' : 'On'}</dd></div>
			</dl>
			<h3 class="fact-group">Conversation</h3>
			<dl>
				<div><dt>Model</dt><dd>{snapshot?.model || '—'}</dd></div>
				<div><dt>Provider</dt><dd>{snapshot?.provider || '—'}</dd></div>
				<div><dt>Mode</dt><dd>{snapshot?.conversationMode || '—'}</dd></div>
				<div><dt>Conversation ID</dt><dd>{snapshot?.conversationId || 'No active archive'}</dd></div>
				<div><dt>Chat key</dt><dd>{snapshot ? (snapshot.chatConfigured ? 'Configured' : 'Needed') : '—'}</dd></div>
				<div><dt>Voice input</dt><dd>{snapshot ? (snapshot.voiceConfigured ? 'Configured' : 'Not set') : '—'}</dd></div>
				<div><dt>Voice state</dt><dd>{snapshot?.voicePhase || '—'}{snapshot?.voiceMuted ? ' · Mic muted' : ''}</dd></div>
				<div><dt>Native readiness</dt><dd>{snapshot?.nativeReadiness.state === 'ready' ? 'Health ready · audio not exercised' : snapshot?.nativeReadiness.state || '—'}</dd></div>
			</dl>
			<h3 class="fact-group">Storage & hardware</h3>
			<dl>
				<div><dt>Local storage</dt><dd>{snapshot?.storage.state || '—'}{snapshot?.storage.durable ? ' · durable' : ' · not persistent'}</dd></div>
				<div><dt>Database</dt><dd>{memory(snapshot?.storage.databaseBytes)}</dd></div>
				<div><dt>Internal free</dt><dd>{memory(snapshot?.internalFree)}</dd></div>
				<div><dt>Largest block</dt><dd>{memory(snapshot?.internalLargest)}</dd></div>
				<div><dt>PSRAM free</dt><dd>{memory(snapshot?.psramFree)}</dd></div>
				<div><dt>Firmware</dt><dd>{snapshot?.firmware || '—'}</dd></div>
			</dl>
            {#if snapshot?.nativeReadiness.error}<p class="notice error" role="alert">{snapshot.nativeReadiness.error}</p>{/if}
            <p class="hint">The selected cloud model receives selected audio and context. Health readiness is not proof of an exercised audio path.</p>
			<p class="hint warn"><strong>Private network only.</strong> This interface uses plain HTTP — don’t expose it to the internet.</p>
			<a class="a-btn ghost" href="/settings">Open settings</a>
		</details>
	</aside>

    <section class="panel-card archive" id="history" aria-labelledby="archive-heading">
        <header class="card-head">
			<div class="head-text"><h2 id="archive-heading">Chat history</h2><p class="card-sub">Past conversations saved on Arvel’s SD card.</p></div>
            <button class="a-btn ghost icon-only" disabled={!ownerReady || historyBusy} onclick={() => { listBack = []; void loadArchive(); }} aria-label="Refresh history" title="Refresh history">
				<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M21 12a9 9 0 1 1-2.64-6.36" /><path d="M21 3v6h-6" /></svg>
			</button>
        </header>
        {#if historyError}<p class="notice error" role="alert">{historyError}</p>{/if}
        {#if historyNotice}<p class="notice" role="status">{historyNotice}</p>{/if}
        <div class="archive-body">
            <nav class="archive-list" aria-label="Saved conversations">
                {#if !ownerReady}<p class="desk-empty">Owner login is required to read local history. <a href="/login?returnTo=home">Log in</a>.</p>
                {:else if !archive}<p class="desk-empty">{historyBusy ? 'Loading conversations…' : 'History hasn’t loaded yet.'}</p>
                {:else if archive.conversations.length === 0}<p class="desk-empty">No saved conversations yet.</p>{/if}
                {#if archive}
                    <ul class="conversation-list">
                        {#each archive.conversations as conversation (conversation.uuid)}
                            {@const active = selectedConversation?.uuid === conversation.uuid}
                            {@const live = conversation.uuid === snapshot?.conversationId}
                            <li><button disabled={historyBusy} class:active aria-pressed={active} onclick={() => { historyKind = 'messages'; historyLiteral = ''; showRecords = false; void inspectConversation(conversation,0,true); }}>
                                <span class="conv-title">{conversation.title || 'Untitled conversation'}</span>
                                <span class="conv-meta">{when(conversation.utc_ms)}{#if live}<span class="pill live">Current</span>{:else if !conversation.closed}<span class="pill">Open</span>{/if}</span>
                            </button></li>
                        {/each}
                    </ul>
                    {#if listBack.length || archive.has_more}
                        <div class="paging">
                            <button class="a-btn ghost" disabled={historyBusy || listBack.length === 0} onclick={() => { const previous = listBack.at(-1) ?? 0; listBack = listBack.slice(0,-1); void loadArchive(previous); }}>Newer</button>
                            <button class="a-btn ghost" disabled={historyBusy || !archive.has_more} onclick={() => { listBack = [...listBack,listCursor]; void loadArchive(archive?.next_cursor); }}>Older</button>
                        </div>
                    {/if}
                {/if}
            </nav>
            <section class="archive-detail" aria-label="Selected conversation">
                {#if !selectedConversation}
                    <div class="empty"><h3>Pick a conversation.</h3><p>Browsing history never starts voice or replays an action.</p></div>
                {:else}
                    <header class="detail-head">
                        <div><h3>{selectedConversation.title || 'Untitled conversation'}</h3><p class="hint">{when(selectedConversation.utc_ms)} · {selectedConversation.closed ? 'Closed' : 'Open'}</p></div>
                        <div class="detail-actions">
                            <button class="a-btn primary" disabled={historyBusy || !connected} onclick={() => void changeMemory('resume',selectedConversation?.uuid)}>Resume</button>
                            {#if !selectedConversation.closed}<button class="a-btn ghost" disabled={historyBusy || !connected} onclick={() => void changeMemory('close',selectedConversation?.uuid)}>Close</button>{/if}
                            <button class="a-btn ghost" disabled={historyBusy} onclick={() => void exportConversation()}>{historyBusy && exportProgress ? `Exporting · ${exportProgress}` : 'Export'}</button>
                            <button class="a-btn ghost danger" disabled={historyBusy || !connected} onclick={() => void changeMemory('delete',selectedConversation?.uuid)}>Delete…</button>
                            {#if historyBusy}<button class="a-btn ghost" onclick={() => memoryController?.abort()}>Cancel</button>{/if}
                        </div>
                    </header>
                    <form class="archive-filter" role="search" onsubmit={(event) => { event.preventDefault(); if (selectedConversation) void inspectConversation(selectedConversation,0,true); }}>
                        <label for="history-literal" class="sr-only">Search messages in this conversation</label>
                        <input id="history-literal" type="search" placeholder="Search this conversation" bind:value={historyLiteral} disabled={historyBusy || historyKind !== 'messages'} oninput={() => { archivedMessages = null; messageBack = []; }} />
                        <button class="a-btn ghost" disabled={historyBusy}>Search</button>
                        <button type="button" class="a-btn ghost" aria-expanded={showRecords} onclick={() => { showRecords = !showRecords; }}>Raw records</button>
                    </form>
                    {#if showRecords}
                        <div class="records-row">
                            <label for="history-kind">Record type</label>
                            <select id="history-kind" bind:value={historyKind} disabled={historyBusy} onchange={() => { archivedMessages = null; archivedEvidence = null; messageBack = []; if (selectedConversation) void inspectConversation(selectedConversation,0,true); }}>{#each historyExportKinds as kind}<option value={kind}>{kind.replaceAll('_',' ')}</option>{/each}</select>
                        </div>
                    {/if}
                    <div class="transcript">
                        {#if archivedMessages}
                            {#if !archivedMessages.messages.length}<p class="desk-empty">{historyLiteral ? 'No messages match that search.' : 'No messages on this page.'}</p>{/if}
                            {#each archivedMessages.messages as message (message.id)}
                                {#if message.role === 'user' || message.role === 'assistant'}
                                    {@render bubble(message.role, message.text, message.utc_ms ? new Date(message.utc_ms).toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' }) : '', message.interrupted, message.tools,
                                        message.sources.map((source) => ({ sourceId: source.id, url: source.url, title: source.title, excerpt: source.excerpt, provider: source.provider, requestId: source.request_id, fetchedUtcMs: source.utc_ms ?? 0, fetchedMonotonicUs: source.monotonic_us, callId: source.call_id, providerSession: source.provider_session, conversationId: source.conversation })))}
                                {:else if message.role === 'tool_unknown'}
                                    <!-- Completed tool rows already show as chips on the reply; only uncertain outcomes need their own line. -->
                                    <p class="tool-line attention">
                                        <svg width="16" height="16" viewBox="0 0 32 32" aria-hidden="true">{@html toolIconMarkup[toolKind(message.tool_name)]}</svg>
                                        Uncertain outcome · {message.tool_name || 'unknown'}{message.text ? ` — ${message.text}` : ''}
                                    </p>
                                {/if}
                            {/each}
                        {:else if archivedEvidence}
                            {#if !archivedEvidence.rows.length}<p class="desk-empty">No {archivedEvidence.kind.replaceAll('_',' ')} on this page.</p>{/if}
                            {#each archivedEvidence.rows as row}<details class="archive-record"><summary>Record {row.id} {typeof row.state === 'string' ? `· ${row.state}` : ''} {typeof row.call_id === 'string' ? `· call ${row.call_id}` : ''}</summary><pre>{JSON.stringify(row,null,2)}</pre></details>{/each}
                        {:else if historyBusy}<p class="desk-empty">Loading…</p>{/if}
                    </div>
                    {#if messageBack.length || archivedMessages?.has_more || archivedEvidence?.has_more}
                        <div class="paging">
                            <button class="a-btn ghost" disabled={historyBusy || messageBack.length === 0} onclick={() => { const previous = messageBack.at(-1) ?? 0; messageBack = messageBack.slice(0,-1); if (selectedConversation) void inspectConversation(selectedConversation,previous); }}>Previous page</button>
                            <button class="a-btn ghost" disabled={historyBusy || !(archivedMessages?.has_more || archivedEvidence?.has_more)} onclick={() => { messageBack = [...messageBack,messageCursor]; if (selectedConversation) void inspectConversation(selectedConversation,archivedMessages?.next_cursor ?? archivedEvidence?.next_cursor ?? 0); }}>Next page</button>
                        </div>
                    {/if}
                {/if}
            </section>
        </div>
        <p class="hint archive-foot">History lives on unencrypted SD, not in this browser. Exports contain private conversation and tool evidence. Deletion is logical, not secure flash erasure. Missing or full storage never triggers an automatic format.</p>
    </section>
</div>

{#snippet bubble(role: 'user' | 'assistant', text: string, time: string, interrupted: boolean, tools: AgentTool[], sources: AgentSource[])}
    {#if role === 'user'}
        <article class="msg user"><p>{text}</p>{#if time}<time>{time}</time>{/if}</article>
    {:else}
        <article class="msg reply">
            <span class="avatar small" aria-hidden="true">{buddyName.slice(0, 1)}</span>
            <div class="reply-body">
                {@render toolList(tools)}
                {#if text}<div class="reply-bubble"><p>{text}</p></div>{/if}
                {#if interrupted}<p class="hint">Interrupted — not every word may have been played.</p>{/if}
                {@render sourceCards(sources)}
                {#if time}<time>{buddyName} · {time}</time>{/if}
            </div>
        </article>
    {/if}
{/snippet}

{#snippet toolList(tools: AgentTool[])}
    {#if tools.length}
        <ul class="tool-list" aria-label="Tool outcomes">
            {#each tools as tool}
                <li class="tool-chip" data-state={tool.state}>
                    <svg width="18" height="18" viewBox="0 0 32 32" aria-hidden="true">{@html toolIconMarkup[toolKind(tool.kind)]}</svg>
                    <span class="tool-name">{tool.verb}{#if tool.target}<span class="tool-target"> {tool.target}</span>{/if}</span>
                    <span class="tool-state">{#if tool.state === 'running'}<span class="spinner small" aria-hidden="true"></span>{/if}{stateLabel[tool.state] ?? tool.state}</span>
                    {#if tool.error || tool.state === 'unknown'}
                        <p class="tool-note">{tool.error}{tool.state === 'unknown' ? ' Outcome is uncertain — check the actual state before repeating a physical action.' : ''}</p>
                    {/if}
                </li>
            {/each}
        </ul>
    {/if}
{/snippet}

{#snippet sourceCards(sources: AgentSource[])}
    {#if sources.length}
        <ul class="sources" aria-label="Sources (untrusted web content)">
            {#each sources as source}
                {@const url = safeSourceUrl(source.url)}
                <li class="source-card">
                    {#if url}<a href={url} target="_blank" rel="noopener noreferrer" referrerpolicy="no-referrer"><span class="source-host">{sourceHost(url)}</span><span class="source-title">{source.title || url}</span></a>
                    {:else}<span class="source-title">{source.title || 'Source'} · unsafe link disabled</span>{/if}
                    {#if source.excerpt}<details><summary>Excerpt</summary><p>{source.excerpt}</p><small>{source.provider} · {source.fetchedUtcMs ? new Date(source.fetchedUtcMs).toLocaleString() : 'time unavailable'} · call {source.callId}</small></details>{/if}
                </li>
            {/each}
        </ul>
    {/if}
{/snippet}

<style>
	.home { max-width: 1180px; margin: 0 auto; padding: 32px; display: grid; grid-template-columns: minmax(0, 1fr) 320px; gap: 20px; align-items: start; }
	.device { grid-column: 1 / -1; display: flex; flex-direction: column; align-items: center; gap: 14px; padding: 8px 0 4px; }
	.device-caption { max-width: 68ch; font-size: 12px; line-height: 1.55; color: var(--sub); text-align: center; }
	.device-caption a { color: var(--accent); font-weight: 600; text-decoration: underline; text-underline-offset: 3px; }
	.remote { display: flex; justify-content: center; align-items: center; flex-wrap: wrap; gap: 4px; padding: 6px; border-radius: 26px; background: var(--paper); box-shadow: var(--shadow-panel); }
	.remote .a-btn { min-height: 40px; }
	.remote .a-btn.ghost { background: transparent; }
	.remote .a-btn.ghost:hover:not(:disabled) { background: var(--chip-bg); }
	.remote .mic-btn { background: var(--mic); }
	.remote .mic-btn:hover:not(:disabled) { background: #8f4c3b; }
	.remote .sep { width: 1px; height: 22px; background: var(--rule-strong); margin: 0 4px; }
	.a-btn.icon-only { padding: 0; width: 40px; }
	.owner-prompt { width: 100%; max-width: 760px; margin: 0; }
	.owner-prompt .a-btn { margin: 8px; min-height: 44px; }
	.owner-prompt p { margin-top: 8px; }
	.inline-retry { font: inherit; color: inherit; border: 0; background: transparent; text-decoration: underline; text-underline-offset: 3px; cursor: pointer; padding: 4px; }
	.sr-only { position: absolute; width: 1px; height: 1px; padding: 0; margin: -1px; overflow: hidden; clip: rect(0, 0, 0, 0); white-space: nowrap; border: 0; }

	.panel-card { background: var(--paper); border-radius: 18px; box-shadow: var(--shadow-panel); min-width: 0; }
	.card-head { display: flex; align-items: center; gap: 12px; padding: 16px 20px; border-bottom: 1px solid var(--rule); }
	.head-text { flex: 1; min-width: 0; }
	h2 { font-size: 15px; font-weight: 650; letter-spacing: -0.2px; }
	.card-sub { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--sub); margin-top: 1px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
	.dot { width: 7px; height: 7px; border-radius: 50%; background: var(--ready); flex-shrink: 0; }
	.dot.busy { background: var(--thinking); }
	.dot.off { background: var(--faint); }
	.avatar { display: grid; place-items: center; width: 36px; height: 36px; border-radius: 50%; background: var(--accent); color: #fff; font-weight: 700; font-size: 15px; flex-shrink: 0; }
	.avatar.small { width: 28px; height: 28px; font-size: 12px; margin-top: 2px; }

	.conversation { display: flex; flex-direction: column; }
	.history { height: clamp(320px, 52vh, 620px); overflow-y: auto; overscroll-behavior: contain; padding: 20px; display: flex; flex-direction: column; gap: 14px; scrollbar-width: thin; }
	.history:focus-visible { outline: 3px solid var(--accent-strong); outline-offset: -3px; }
	.empty { margin: auto; max-width: 42ch; text-align: center; padding: 24px 0; }
	.empty h3 { font-size: 19px; font-weight: 650; letter-spacing: -0.4px; margin-bottom: 6px; }
	.empty p { font-size: 14px; line-height: 1.45; color: var(--sub); }
	.empty .a-btn { margin-top: 16px; }

	.msg p { white-space: pre-wrap; overflow-wrap: anywhere; }
	.msg time { display: block; font-size: 11px; color: var(--faint); margin-top: 4px; }
	.msg.user { align-self: flex-end; max-width: 78%; }
	.msg.user p { background: var(--accent); color: #fff; font-size: 14px; line-height: 1.45; padding: 9px 14px; border-radius: 18px 18px 4px 18px; }
	.msg.user time { text-align: right; }
	.msg.reply { display: flex; gap: 10px; align-items: flex-start; max-width: 88%; }
	.reply-body { display: flex; flex-direction: column; gap: 6px; min-width: 0; }
	.reply-bubble { background: #fff; border: 1px solid var(--rule); border-radius: 4px 18px 18px 18px; padding: 9px 14px; }
	.reply-bubble p { font-size: 14.5px; line-height: 1.5; color: var(--ink); }
	.typing { display: flex; gap: 4px; padding: 10px 14px; margin-left: 38px; width: fit-content; background: #fff; border: 1px solid var(--rule); border-radius: 4px 18px 18px 18px; }
	.typing span { width: 6px; height: 6px; border-radius: 50%; background: var(--faint); animation: bounce 1.2s infinite ease-in-out; }
	.typing span:nth-child(2) { animation-delay: 0.15s; }
	.typing span:nth-child(3) { animation-delay: 0.3s; }
	@keyframes bounce { 0%, 60%, 100% { transform: translateY(0); opacity: 0.5; } 30% { transform: translateY(-4px); opacity: 1; } }
	.live-tools { margin-left: 38px; }

	.tool-list, .sources { list-style: none; padding: 0; display: flex; flex-wrap: wrap; gap: 6px; }
	.tool-chip { display: inline-flex; flex-wrap: wrap; align-items: center; gap: 6px; max-width: 100%; padding: 4px 10px 4px 5px; border-radius: 14px; background: var(--chip-bg); font-size: 12.5px; }
	.tool-chip svg { flex-shrink: 0; border-radius: 5px; }
	.tool-name { font-weight: 600; color: var(--ink); overflow-wrap: anywhere; }
	.tool-target { font-weight: 450; color: var(--sub); margin-left: 4px; }
	.tool-state { display: inline-flex; align-items: center; gap: 4px; font-size: 11px; font-weight: 600; color: var(--sub); }
	.tool-chip[data-state='succeeded'] .tool-state { color: #3f6b5a; }
	.tool-chip[data-state='failed'], .tool-chip[data-state='unknown'], .tool-chip[data-state='partial'] { background: #f6e6e1; }
	.tool-chip[data-state='failed'] .tool-state, .tool-chip[data-state='unknown'] .tool-state, .tool-chip[data-state='partial'] .tool-state { color: #7a3a2c; }
	.tool-note { flex-basis: 100%; font-size: 12px; line-height: 1.45; color: #7a3a2c; padding: 0 0 2px 24px; }
	.source-card { display: flex; flex-direction: column; gap: 2px; min-width: 0; max-width: 260px; padding: 8px 12px; border: 1px solid var(--rule); border-radius: 12px; background: #fff; font-size: 12.5px; }
	.source-card a { display: flex; flex-direction: column; color: var(--ink); text-decoration: none; }
	.source-card a:hover .source-title { text-decoration: underline; text-underline-offset: 2px; }
	.source-host { font-size: 11px; color: var(--sub); }
	.source-title { font-weight: 600; overflow: hidden; text-overflow: ellipsis; display: -webkit-box; -webkit-line-clamp: 2; line-clamp: 2; -webkit-box-orient: vertical; }
	.source-card summary { font-size: 11px; color: var(--sub); cursor: pointer; margin-top: 2px; }
	.source-card details p { margin: 4px 0; line-height: 1.45; overflow-wrap: anywhere; }
	.source-card small { color: var(--faint); overflow-wrap: anywhere; }

	.composer { padding: 12px 16px 14px; border-top: 1px solid var(--rule); }
	.composer-box { display: flex; align-items: flex-end; gap: 8px; padding: 6px 6px 6px 14px; background: #fff; border: 1px solid var(--rule-strong); border-radius: 22px; transition: border-color 120ms ease-out; }
	.composer-box:focus-within { border-color: var(--accent); }
	.composer-box.over { border-color: var(--mic); }
	textarea { flex: 1; min-height: 40px; max-height: 240px; resize: none; field-sizing: content; background: transparent; border: none; color: var(--ink); padding: 8px 0; font: inherit; font-size: 14.5px; line-height: 1.45; }
	textarea:focus { outline: none; }
	.send { display: grid; place-items: center; width: 38px; height: 38px; border-radius: 50%; border: none; background: var(--accent); color: #fff; cursor: pointer; flex-shrink: 0; transition: background-color 120ms ease-out; }
	.send:hover:not(:disabled) { background: var(--accent-strong); }
	.composer-help { display: flex; justify-content: space-between; gap: 12px; margin: 6px 6px 0; }
	.composer-help .over { color: #8a3b2c; font-weight: 600; }
	.spinner { width: 14px; height: 14px; border-radius: 50%; border: 2px solid currentColor; border-right-color: transparent; animation: spin 0.8s linear infinite; }
	.spinner.small { width: 9px; height: 9px; border-width: 1.5px; }
	@keyframes spin { to { transform: rotate(360deg); } }
	.hint { font-size: 12px; color: var(--sub); line-height: 1.45; }
	.hint a { color: var(--accent); font-weight: 600; text-decoration: underline; text-underline-offset: 3px; }
	.hint.state-label:empty { display: none; }
	.hint.warn { border-left: 3px solid var(--mic); padding-left: 10px; margin: 12px 0 16px; }
	.composer .hint + .hint { margin-top: 4px; }
	.notice { margin: 12px 20px 0; }

	.side { display: flex; flex-direction: column; gap: 16px; min-width: 0; }
	.desk-card { padding: 16px 18px; }
	.desk-head { display: flex; align-items: center; gap: 10px; margin-bottom: 12px; }
	.desk-head h2 { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
	.desk-icon { flex-shrink: 0; }
	.desk-meta { font-size: 12px; color: var(--sub); }
	.desk-empty { font-size: 13px; line-height: 1.5; color: var(--sub); }
	.desk-empty a { color: var(--accent); font-weight: 600; text-decoration: underline; }
	.desk-foot { font-size: 11.5px; color: var(--faint); margin-top: 12px; }
	.pill { font-size: 11px; font-weight: 650; padding: 2px 8px; border-radius: 10px; background: var(--accent-soft); color: var(--accent-strong); white-space: nowrap; }
	.pill.paused { background: var(--chip-bg); color: var(--chip-ink); }
	.pill.done { background: #f6e6e1; color: #7a3a2c; }
	.pill.live { background: var(--accent); color: #fff; }

	.timer-clock { font-size: 40px; font-weight: 650; letter-spacing: -1px; font-variant-numeric: tabular-nums; line-height: 1; margin-bottom: 12px; }
	.progress { height: 6px; border-radius: 3px; background: var(--rule); overflow: hidden; margin-bottom: 8px; }
	.progress span { display: block; height: 100%; background: #e5674f; border-radius: 3px; transition: width 1s linear; }

	.weather-now { display: flex; align-items: center; gap: 14px; }
	.temp { font-size: 40px; font-weight: 650; letter-spacing: -1px; line-height: 1; }
	.condition { font-size: 14px; font-weight: 600; margin-bottom: 2px; }
	.forecast { list-style: none; padding: 12px 0 0; margin: 12px 0 0; border-top: 1px solid var(--rule); display: grid; grid-template-columns: repeat(4, 1fr); gap: 4px; text-align: center; }
	.forecast li { display: flex; flex-direction: column; gap: 2px; font-size: 13px; }
	.rain { font-size: 11px; color: var(--faint); }
	.rain.wet { color: #3b6f99; font-weight: 600; }

	.task-list, .note-list { list-style: none; padding: 0; display: flex; flex-direction: column; }
	.task-list li { display: flex; align-items: flex-start; gap: 10px; padding: 8px 0; border-bottom: 1px solid var(--rule); font-size: 13.5px; line-height: 1.4; }
	.task-list li:last-child { border-bottom: none; }
	.check { display: grid; place-items: center; width: 17px; height: 17px; border-radius: 5px; border: 1.5px solid var(--rule-strong); flex-shrink: 0; margin-top: 1px; color: #fff; }
	.task-list .completed .check { background: var(--ready); border-color: var(--ready); }
	.task-text { flex: 1; min-width: 0; overflow-wrap: anywhere; }
	.task-list .completed .task-text { color: var(--faint); text-decoration: line-through; }
	.due { font-size: 11px; font-weight: 600; color: var(--sub); white-space: nowrap; padding-top: 2px; }
	.due.overdue { color: #8a3b2c; }
	.task-list + .note-list { margin-top: 12px; }
	.note-list { gap: 8px; }
	.note-list li { padding: 10px 12px; border-radius: 10px; background: #fbf3dc; font-size: 13.5px; line-height: 1.45; }
	.note-list p { white-space: pre-wrap; overflow-wrap: anywhere; }
	.note-list .due { display: block; margin-top: 4px; }

	.facts { padding: 4px 18px; }
	.facts[open] { padding-bottom: 16px; }
	.facts summary { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 12px 0; cursor: pointer; list-style: none; }
	.facts summary::-webkit-details-marker { display: none; }
	.facts summary::after { content: ''; width: 7px; height: 7px; border-right: 2px solid var(--sub); border-bottom: 2px solid var(--sub); transform: rotate(45deg); margin: -4px 4px 0 0; transition: transform 120ms ease-out; }
	.facts[open] summary::after { transform: rotate(225deg); margin-top: 4px; }
	.facts summary .desk-meta { margin-left: auto; }
	.fact-group { margin: 14px 0 4px; color: var(--sub); font-size: 11px; font-weight: 700; letter-spacing: 0.6px; text-transform: uppercase; }
	dl { margin: 0 0 4px; font-size: 13px; }
	dl > div { display: flex; justify-content: space-between; gap: 12px; padding: 6px 0; border-bottom: 1px solid var(--rule); }
	dl > div:last-child { border-bottom: none; }
	dt { color: var(--sub); flex-shrink: 0; }
	dd { text-align: right; font-weight: 550; overflow-wrap: anywhere; min-width: 0; }

    .archive { grid-column: 1 / -1; scroll-margin-top: 24px; }
    .archive-body { display: grid; grid-template-columns: 280px minmax(0, 1fr); min-height: 420px; }
    .archive-list { border-right: 1px solid var(--rule); padding: 12px; display: flex; flex-direction: column; gap: 8px; min-width: 0; }
    .archive-list .desk-empty { padding: 8px; }
    .conversation-list { list-style: none; padding: 0; display: flex; flex-direction: column; gap: 2px; max-height: 560px; overflow-y: auto; scrollbar-width: thin; }
    .conversation-list button { width: 100%; display: flex; flex-direction: column; gap: 3px; text-align: left; padding: 10px 12px; border: none; border-radius: 12px; background: transparent; color: var(--ink); font: inherit; cursor: pointer; }
    .conversation-list button:hover:not(:disabled) { background: var(--chip-bg); }
    .conversation-list button.active { background: var(--accent-soft); }
    .conv-title { font-size: 13.5px; font-weight: 600; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .conv-meta { display: flex; align-items: center; gap: 6px; font-size: 11.5px; color: var(--sub); }
    .archive-detail { padding: 16px 20px; display: flex; flex-direction: column; gap: 12px; min-width: 0; }
    .detail-head { display: flex; flex-wrap: wrap; justify-content: space-between; align-items: flex-start; gap: 12px; }
    .detail-head h3 { font-size: 16px; font-weight: 650; letter-spacing: -0.2px; overflow-wrap: anywhere; }
    .detail-actions { display: flex; flex-wrap: wrap; gap: 6px; }
    .detail-actions .a-btn { min-height: 36px; padding: 0 14px; }
    .a-btn.danger { color: #7a3a2c; }
    .a-btn.danger:hover:not(:disabled) { background: #f6e6e1; }
    .archive-filter { display: flex; gap: 6px; }
    .archive-filter input { flex: 1; min-width: 0; min-height: 40px; color: var(--ink); background: #fff; border: 1px solid var(--rule-strong); border-radius: 20px; padding: 0 16px; font: inherit; font-size: 13.5px; }
    .archive-filter input:focus { outline: none; border-color: var(--accent); }
    .records-row { display: flex; align-items: center; gap: 8px; font-size: 13px; }
    .records-row select { min-height: 36px; color: var(--ink); background: #fff; border: 1px solid var(--rule-strong); border-radius: 10px; padding: 0 10px; font: inherit; }
    .transcript { display: flex; flex-direction: column; gap: 14px; padding: 16px; border-radius: 14px; background: var(--page); max-height: 560px; overflow-y: auto; scrollbar-width: thin; }
    .transcript .reply-bubble { background: var(--paper); }
    .tool-line { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--sub); margin-left: 38px; overflow-wrap: anywhere; }
    .tool-line.attention { color: #7a3a2c; }
    .archive-record { background: var(--paper); border-radius: 10px; padding: 4px 12px; overflow-wrap: anywhere; }
    .archive-record summary { cursor: pointer; padding: 8px 0; font-size: 13px; }
    .archive-record pre { white-space: pre-wrap; overflow-wrap: anywhere; font-size: 12px; line-height: 1.5; max-height: 360px; overflow: auto; }
    .paging { display: flex; flex-wrap: wrap; gap: 8px; }
    .archive-foot { padding: 12px 20px 16px; border-top: 1px solid var(--rule); }
    .archive .notice { margin: 12px 20px 0; }

    .home :is(button,a,input,select,textarea,summary):focus-visible { outline: 3px solid var(--accent-strong); outline-offset: 3px; }
    .home button:disabled { opacity: 0.5; cursor: not-allowed; }
	@media (max-width: 960px) {
		.home { grid-template-columns: minmax(0, 1fr); padding: 20px 16px; }
		.side { display: grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); }
	}
	@media (max-width: 720px) {
		.archive-body { grid-template-columns: minmax(0, 1fr); }
		.archive-list { border-right: none; border-bottom: 1px solid var(--rule); }
		.conversation-list { max-height: 240px; }
	}
	@media (max-width: 560px) {
		.remote { border-radius: 16px; }
		.remote .sep { display: none; }
		.remote .a-btn { flex: 1 1 calc(50% - 6px); min-height: 44px; }
		.card-head, .history, .archive-detail { padding-left: 14px; padding-right: 14px; }
		.msg.user, .msg.reply { max-width: 100%; }
	}
	@media (prefers-reduced-motion: reduce) {
		.typing span, .spinner { animation: none; }
		.progress span { transition: none; }
	}
</style>
