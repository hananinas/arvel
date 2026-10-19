<script lang="ts">
	import { onMount, tick } from 'svelte';
	import NativeStatePreview from '$lib/components/NativeStatePreview.svelte';
    import { DeviceRequestError, deviceRequest, isAgentSnapshot, safeSourceUrl, fetchHistory, fetchHistoryPage,
        fetchHistoryExportPage, memoryAction, historyExportKinds, type AgentSnapshot, type AgentTool,
        type AgentSource, type ConversationPage, type ConversationSummary, type HistoryPage, type HistoryRowsPage,
        type HistoryExportKind } from '$lib/agent';
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
            for (const controller of requests) controller.abort();
            memoryController?.abort();
            archive = null; selectedConversation = null; archivedMessages = null; archivedEvidence = null;
            historyNotice = ''; historyError = ''; exportProgress = 0; exportFinished = false;
        }
    });
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
		<div class="case">
			<div class="bezel">
				<NativeStatePreview snapshot={ownerReady && connected ? snapshot : null} authenticated={ownerReady} {loading} />
			</div>
		</div>
		<p class="device-caption">A drawn preview of what is happening on physical Arvel, not a screen capture. Remote voice uses <strong>Arvel’s physical microphone</strong>; this page never opens your browser microphone. <a href="/settings#speech-output">Manage voice in Settings</a>.</p>
		<div class="remote-controls" aria-label="Physical device controls">
			<button type="button" class="a-btn primary" disabled={voiceToggleDisabled} onclick={() => control('voice_toggle')}>{voiceToggleLabel}</button>
            <button type="button" class="a-btn ghost" disabled={!ownerReady || !connected || !snapshot || stopping} onclick={() => control('voice_stop')}>{stopping ? 'Stopping…' : snapshot?.voiceActive ? 'End voice session / Stop' : snapshot?.voiceConnecting ? 'Cancel connection / Stop' : 'Stop current activity'}</button>
			<button type="button" class="a-btn ghost" disabled={!canControl} onclick={() => control('screen_toggle')}>{snapshot?.screenOff ? 'Turn screen on' : 'Toggle screen'}</button>
			<button type="button" class="a-btn ghost" disabled={!canControl} onclick={() => control('brightness_cycle')}>Cycle brightness{snapshot ? ` · ${snapshot.preferences.brightness}%` : ''}</button>
			<button type="button" class="a-btn ghost" disabled={loading || polling} onclick={() => void refresh()}>Refresh access & status</button>
		</div>
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
			<div>
				<h2 id="conversation-heading" tabindex="-1">Conversation</h2>
				<p class="card-sub">The native conversation on Arvel, shared with its physical screen. Text sends silently; device voice is controlled separately above.</p>
			</div>
			<a class="a-btn ghost" href="#conversation-history">Read recent conversation</a>
            <button class="a-btn ghost" onclick={resetConversation} disabled={!canControl || !snapshot || historyBusy}>{historyBusy ? 'Updating memory…' : 'New chat'}</button>
		</header>
		{#if connectionError}<div class="notice error" role="alert"><strong>{loginNeeded ? 'Sign in again.' : 'Connection needs attention.'}</strong> {connectionError} {#if loginNeeded}<a href="/login?returnTo=home">Log in to Arvel</a>{:else}<button type="button" class="inline-retry" onclick={() => void refresh()}>Retry connection</button> Check that Arvel is on your network and that the localhost proxy points to its address.{/if}</div>{/if}
		{#if requestError}<div class="notice error" role="alert"><strong>Request needs attention.</strong> {requestError}</div>{/if}
		{#if snapshot?.lastError}<div class="notice error" role="alert"><strong>Arvel reported an error.</strong> {snapshot.lastError} {#if snapshot.voicePhase === 'error'}Stop device voice above, <a href="/settings#speech-output">check the voice service settings</a>, then start device voice again when ready.{:else}<a href="/settings#chat-provider">Check the configured service settings</a> and the device’s connection before trying again.{/if}</div>{/if}
		{#if snapshot && !snapshot.chatConfigured && snapshot.conversationMode === 'text_pipeline'}<div class="notice">The saved text profile needs an API key. <a href="/settings#chat-provider">Open Settings</a> to add it — never paste keys into the conversation.</div>{/if}
		{#if snapshot && !snapshot.chatConfigured && snapshot.conversationMode === 'codex_live'}<div class="notice"><strong>Codex device tools need a chat backend.</strong> <a href="/settings#chat-provider">Configure the saved text profile</a> to use device actions and search through Codex delegations. Ordinary Codex conversation uses its saved account; this text profile does not require speech-to-text or change the live voice transport.</div>{/if}
		<div id="conversation-history" class="history" bind:this={historyElement} role="log" aria-label="Recent conversation" aria-live="polite" aria-relevant="additions text" tabindex="-1">
			{#if !snapshot}
				<div class="empty"><h3>{loading ? 'Connecting to Arvel…' : loginNeeded ? 'Your device, owner access.' : 'Arvel is out of reach.'}</h3><p>{loading ? 'Checking owner access, then reading the native device conversation.' : loginNeeded ? 'Log in to view the device state and use the native conversation. Your draft stays in this tab.' : 'Check the device connection or localhost proxy, then refresh. Your draft stays in this tab.'}</p>{#if loginNeeded}<a class="a-btn primary" href="/login?returnTo=home">Log in to Arvel</a>{/if}</div>
			{:else if snapshot.messages.length === 0}
				<div class="empty"><h3>Nothing here yet.</h3><p>Send a text message below, or start device voice with permission from people near Arvel.</p></div>
			{:else}
				{#each snapshot.messages as message (message.id)}
					{#if message.role === 'user'}
						<article class="msg user"><p>{message.text}</p>{#if message.time}<time>{message.time}</time>{/if}</article>
					{:else}
                        <article class="msg reply"><span class="bar" aria-hidden="true"></span><div><p class="who">{buddyName}<span>AI</span>{#if message.time}<time>{message.time}</time>{/if}</p><p>{message.text}</p>
                            {#if message.interrupted}<p class="hint">Interrupted output — this transcript does not mean every word was played.</p>{/if}
                            {@render toolList(message.tools ?? [])}
                            {@render sourceCards(message.sources ?? [])}
                        </div></article>
					{/if}
				{/each}
			{/if}
            {#if snapshot}
                <section aria-label="Current tool progress" aria-live="polite">
                    {@render toolList(snapshot.tools)}
                    {@render sourceCards(snapshot.sources)}
                </section>
            {/if}
		</div>
		<form class="composer" onsubmit={sendMessage}>
			<div class="composer-label"><label for="buddy-message">Message {buddyName}</label><span class:over={draftBytes > 8192}>{draftBytes.toLocaleString()} / 8,192 bytes</span></div>
			<textarea id="buddy-message" bind:this={composerElement} bind:value={draft} rows="3" aria-describedby="composer-help" aria-invalid={draftBytes > 8192} oninput={(event) => { draft = event.currentTarget.value; saveDraft(); }} onkeydown={(event) => { if (event.ctrlKey && event.key === 'Enter' && !event.isComposing) { event.preventDefault(); void sendMessage(); } }}></textarea>
			<div class="composer-actions">
				<button type="submit" class="a-btn primary" disabled={!canSend}>{pending === 'send' ? 'Sending…' : 'Send'}</button>
			</div>
			<p id="composer-help" class="hint">{draftBytes > 8192 ? 'Too long — shorten it to 8,192 UTF-8 bytes.' : 'Ctrl+Enter to send. Your draft stays in this tab until Arvel accepts it. This browser never records your microphone.'}</p>
			{#if snapshot && snapshot.conversationMode !== 'text_pipeline'}<p class="hint">This live integration accepts speech, not typed conversation or arbitrary text read-aloud here. <a href="/settings#conversation-model">Explicitly switch to your saved text/read-aloud profile</a> to use this draft. No request is silently routed to another model.</p>{/if}
			{#if snapshot?.voiceActive || snapshot?.voiceConnecting}<p class="hint">Stop device voice before sending text or starting a new chat.</p>{/if}
			<p class="hint state-label" role="status" aria-live="polite">{notice}</p>
		</form>
	</section>

	<aside class="panel-card facts" aria-labelledby="device-heading">
		<h2 id="device-heading">On your desk</h2>
		<dl>
			<div><dt>Status</dt><dd>{loginNeeded ? 'Login required' : !connected ? (loading ? 'Connecting' : 'Unreachable') : snapshot?.status || 'Connected'}</dd></div>
			<div><dt>Wi-Fi</dt><dd>{snapshot ? (snapshot.wifiConnected ? snapshot.ssid || 'Connected' : 'Offline') : '—'}</dd></div>
			<div><dt>Address</dt><dd>{snapshot?.ip || '—'}</dd></div>
			<div><dt>Clock</dt><dd>{snapshot ? (snapshot.clockReady ? snapshot.localTime || 'Synced' : 'Syncing') : '—'}</dd></div>
			<div><dt>Model</dt><dd>{snapshot?.model || '—'}</dd></div>
            <div><dt>Provider</dt><dd>{snapshot?.provider || '—'}</dd></div>
            <div><dt>Conversation mode</dt><dd>{snapshot?.conversationMode || '—'}</dd></div>
            <div><dt>Conversation ID</dt><dd>{snapshot?.conversationId || 'No active archive'}</dd></div>
            <div><dt>Local storage</dt><dd>{snapshot?.storage.state || '—'}{snapshot?.storage.durable ? ' · Durable archive available' : ' · Persistence unavailable'}</dd></div>
            <div><dt>Database</dt><dd>{memory(snapshot?.storage.databaseBytes)}</dd></div>
            <div><dt>Native readiness</dt><dd>{snapshot?.nativeReadiness.state === 'ready' ? 'Authenticated health ready · audio not exercised' : snapshot?.nativeReadiness.state || '—'}</dd></div>
			<div><dt>Chat key</dt><dd>{snapshot ? (snapshot.chatConfigured ? 'Configured' : 'Needed') : '—'}</dd></div>
			<div><dt>Voice input</dt><dd>{snapshot ? (snapshot.voiceConfigured ? 'Configured' : 'Not set') : '—'}</dd></div>
			<div><dt>Voice state</dt><dd>{snapshot?.voicePhase || '—'}{snapshot?.voiceMuted ? ' · Mic muted' : ''}</dd></div>
			<div><dt>Screen</dt><dd>{snapshot?.screenOff === undefined ? '—' : snapshot.screenOff ? 'Off' : 'On'}</dd></div>
			<div><dt>Internal free</dt><dd>{memory(snapshot?.internalFree)}</dd></div>
			<div><dt>Largest block</dt><dd>{memory(snapshot?.internalLargest)}</dd></div>
			<div><dt>PSRAM free</dt><dd>{memory(snapshot?.psramFree)}</dd></div>
			<div><dt>Firmware</dt><dd>{snapshot?.firmware || '—'}</dd></div>
		</dl>
        {#if snapshot?.nativeReadiness.error}<p class="notice error" role="alert">{snapshot.nativeReadiness.error}</p>{/if}
        <p class="hint">The selected cloud model receives selected audio and context. Native audio provides its own speech; the text pipeline uses explicit STT/text/TTS. Muting gates the microphone, not replies; End closes voice. Health readiness is not proof of an exercised audio or home-control path.</p>
		<p class="hint warn"><strong>Private network only.</strong> This interface uses plain HTTP — don’t expose it to the internet.</p>
		<a class="a-btn ghost" href="/settings">Open settings</a>
	</aside>
    <section class="panel-card archive" id="history" aria-labelledby="archive-heading">
        <header class="card-head"><div><h2 id="archive-heading">Local conversation memory</h2><p class="card-sub">Inspect 20 records at a time. New and closed chats remain here until owner deletion or a visible storage limit.</p></div>
            <button class="a-btn ghost" disabled={!ownerReady || historyBusy} onclick={() => { listBack = []; void loadArchive(); }}>{historyBusy ? 'Loading…' : 'Refresh history'}</button>
        </header>
        <div class="archive-body">
            <p class="hint">History is on unencrypted SD, not in this browser’s storage. SD/NVS and this LAN HTTP interface are not end-to-end encrypted. Exports contain private conversation and tool evidence; keep them safe. Deletion is logical, not secure flash erasure. Missing/full/error storage never means an automatic format.</p>
            {#if !ownerReady}<p>Owner login is required to inspect local memory. <a href="/login?returnTo=home">Log in</a>.</p>{/if}
            {#if historyError}<p class="notice error" role="alert">{historyError}</p>{/if}
            <p class="hint" role="status">{historyNotice}</p>
            {#if archive}
                {#if archive.conversations.length === 0}<p>No conversations on this page.</p>{/if}
                <ul class="conversation-list">
                    {#each archive.conversations as conversation (conversation.uuid)}
                        <li><button class="a-btn ghost" disabled={historyBusy} aria-pressed={selectedConversation?.uuid === conversation.uuid} onclick={() => { historyKind = 'messages'; historyLiteral = ''; void inspectConversation(conversation,0,true); }}>{conversation.title || 'Untitled conversation'} · {conversation.closed ? 'Closed' : 'Open'}<small>{conversation.uuid} · {conversation.utc_ms ? new Date(conversation.utc_ms).toLocaleString() : 'UTC unavailable'}</small></button></li>
                    {/each}
                </ul>
                <div class="paging">
                    <button class="a-btn ghost" disabled={historyBusy || listBack.length === 0} onclick={() => { const previous = listBack.at(-1) ?? 0; listBack = listBack.slice(0,-1); void loadArchive(previous); }}>Previous conversations</button>
                    <button class="a-btn ghost" disabled={historyBusy || !archive.has_more} onclick={() => { listBack = [...listBack,listCursor]; void loadArchive(archive?.next_cursor); }}>Next conversations</button>
                </div>
            {:else if ownerReady}<p>Choose Refresh history to read the SD archive. Browsing does not start voice or replay actions.</p>{/if}
            {#if selectedConversation}
                <section class="archive-detail" aria-label="Selected conversation">
                    <h3>{selectedConversation.title || 'Untitled conversation'}</h3><p class="hint">{selectedConversation.uuid}</p>
                    <div class="paging">
                        <button class="a-btn primary" disabled={historyBusy || !connected} onclick={() => void changeMemory('resume',selectedConversation?.uuid)}>Resume conversation</button>
                        <button class="a-btn ghost" disabled={historyBusy || !connected} onclick={() => void changeMemory('close',selectedConversation?.uuid)}>Close conversation</button>
                        <button class="a-btn ghost" disabled={historyBusy || !connected} onclick={() => void changeMemory('delete',selectedConversation?.uuid)}>Delete / forget…</button>
                    </div>
                    <form class="archive-filter" onsubmit={(event) => { event.preventDefault(); if (selectedConversation) void inspectConversation(selectedConversation,0,true); }}>
                        <label for="history-kind">Records</label><select id="history-kind" bind:value={historyKind} disabled={historyBusy} onchange={() => { archivedMessages = null; archivedEvidence = null; messageBack = []; }}>{#each historyExportKinds as kind}<option value={kind}>{kind.replaceAll('_',' ')}</option>{/each}</select>
                        <label for="history-literal">Literal message search (256 UTF-8 bytes)</label><input id="history-literal" bind:value={historyLiteral} disabled={historyBusy || historyKind !== 'messages'} oninput={() => { archivedMessages = null; messageBack = []; }} />
                        <button class="a-btn ghost" disabled={historyBusy}>Inspect records</button>
                    </form>
                    {#if archivedMessages}
                        {#if !archivedMessages.messages.length}<p>No messages match this page.</p>{/if}
                        {#each archivedMessages.messages as message (message.id)}
                            <article class="archive-record"><h4>{message.role} · message {message.id}{message.interrupted ? ' · Interrupted' : ''}</h4>
                                <p class="hint">{message.utc_ms ? new Date(message.utc_ms).toLocaleString() : 'UTC unavailable'} · session {message.provider_session}{message.call_id ? ` · call ${message.call_id} · ${message.tool_name}` : ''}</p><p>{message.text}</p>
                                {@render toolList(message.tools)}
                                {#each message.sources as source}<div class="source-card"><strong>Source {source.id}</strong>
                                    {#if safeSourceUrl(source.url)}<a href={safeSourceUrl(source.url) ?? undefined} target="_blank" rel="noopener noreferrer" referrerpolicy="no-referrer">{source.title || source.url}</a>{:else}<p>Unsafe source URL — link disabled.</p>{/if}
                                    <p>{source.excerpt}</p><small>{source.provider} · {source.utc_ms ? new Date(source.utc_ms).toLocaleString() : 'UTC unavailable'} · monotonic {source.monotonic_us} µs · boot {source.boot_uuid} · call {source.call_id} · session {source.provider_session}{source.request_id ? ` · request ${source.request_id}` : ' · provider request ID unavailable'}</small>
                                </div>{/each}
                            </article>
                        {/each}
                    {:else if archivedEvidence}
                        {#if !archivedEvidence.rows.length}<p>No {archivedEvidence.kind.replaceAll('_',' ')} on this page.</p>{/if}
                        {#each archivedEvidence.rows as row}<details class="archive-record"><summary>Record {row.id} {typeof row.state === 'string' ? `· ${row.state}` : ''} {typeof row.call_id === 'string' ? `· call ${row.call_id}` : ''}</summary><pre>{JSON.stringify(row,null,2)}</pre></details>{/each}
                    {/if}
                    <div class="paging">
                        <button class="a-btn ghost" disabled={historyBusy || messageBack.length === 0} onclick={() => { const previous = messageBack.at(-1) ?? 0; messageBack = messageBack.slice(0,-1); if (selectedConversation) void inspectConversation(selectedConversation,previous); }}>Previous records</button>
                        <button class="a-btn ghost" disabled={historyBusy || !(archivedMessages?.has_more || archivedEvidence?.has_more)} onclick={() => { messageBack = [...messageBack,messageCursor]; if (selectedConversation) void inspectConversation(selectedConversation,archivedMessages?.next_cursor ?? archivedEvidence?.next_cursor ?? 0); }}>Next records</button>
                    </div>
                    <h3>Export this conversation</h3><p class="hint">Stream messages, session boundaries, tool calls, sources and tool evidence into one JSON Lines file. Device requests stay bounded to 20 rows / 48 KiB. The browser stores the download; close the conversation first for a stable archive.</p>
                    <div class="paging"><button class="a-btn ghost" disabled={historyBusy} onclick={() => void exportConversation()}>{historyBusy && exportProgress ? `Exporting · ${exportProgress} pages` : exportFinished ? 'Download offered · export again' : 'Export conversation'}</button>
                        {#if historyBusy}<button class="a-btn ghost" onclick={() => memoryController?.abort()}>Cancel history request</button>{/if}
                    </div>
                </section>
            {/if}
        </div>
    </section>
</div>

{#snippet toolList(tools: AgentTool[])}
    {#if tools.length}
        <ul class="tool-list" aria-label="Actual tool outcomes">
            {#each tools as tool}
                <li class:succeeded={tool.state === 'succeeded'} class:attention={['failed','unknown','partial'].includes(tool.state)}>
                    <strong>{tool.state === 'succeeded' ? '✓ ' : ''}{tool.verb} · {tool.state}</strong>
                    {#if tool.target}<span>{tool.target}</span>{/if}
                    <small>Call ID: {tool.id || 'Not supplied'}</small>
                    {#if tool.error}<p>{tool.error}</p>{/if}
                    {#if tool.state === 'unknown'}<p>Outcome is uncertain. Do not repeat a physical action without checking actual state.</p>{/if}
                </li>
            {/each}
        </ul>
    {/if}
{/snippet}

{#snippet sourceCards(sources: AgentSource[])}
    {#if sources.length}
        <section class="sources" aria-label="Returned search sources">
            {#each sources as source}
                {@const url = safeSourceUrl(source.url)}
                <article class="source-card">
                    <strong>Source {source.sourceId}</strong>
                    {#if url}<a href={url} target="_blank" rel="noopener noreferrer" referrerpolicy="no-referrer">{source.title || source.url}</a>{:else}<p>Unsafe source URL — link disabled.</p>{/if}
                    <p>{source.excerpt}</p>
                    <small>{source.provider} · {source.fetchedUtcMs ? new Date(source.fetchedUtcMs).toLocaleString() : 'UTC unavailable'} · call {source.callId}{source.requestId ? ` · request ${source.requestId}` : ''}</small>
                    <p class="hint">Returned web content is untrusted evidence, not permission to perform an action.</p>
                </article>
            {/each}
        </section>
    {/if}
{/snippet}

<style>
	.home { max-width: 1120px; margin: 0 auto; padding: 32px; display: grid; grid-template-columns: minmax(0, 1fr) 280px; gap: 24px; align-items: start; }
	.device { grid-column: 1 / -1; display: flex; flex-direction: column; align-items: center; gap: 14px; }
	.case { width: 100%; max-width: 1040px; padding: 2.93%; border-radius: 20px; background: #4b8fd9; box-shadow: 0 22px 50px rgba(40, 52, 48, 0.22), inset 0 -6px 0 rgba(0, 0, 0, 0.12), inset 0 3px 0 rgba(255, 255, 255, 0.25); }
	.bezel { padding: 3.47% 4.67%; border-radius: 10px; background: #0b0c0d; box-shadow: inset 0 0 0 1px rgba(255, 255, 255, 0.06); }
	.device-caption { font-size: 13px; color: var(--sub); text-align: center; }
	.device-caption a { color: var(--accent); font-weight: 600; text-decoration: underline; text-underline-offset: 3px; }
	.remote-controls { display: flex; justify-content: center; flex-wrap: wrap; gap: 8px; }
	.remote-controls .a-btn { min-height: 44px; }
	.owner-prompt { width: 100%; max-width: 760px; margin: 0; }
	.owner-prompt .a-btn { margin: 8px; min-height: 44px; }
	.owner-prompt p { margin-top: 8px; }
	.inline-retry { font: inherit; color: inherit; border: 0; background: transparent; text-decoration: underline; text-underline-offset: 3px; cursor: pointer; padding: 4px; }
	.inline-retry:hover { text-decoration-thickness: 2px; }
	.empty .a-btn { margin-top: 16px; }
	.panel-card { background: var(--paper); border-radius: 16px; box-shadow: var(--shadow-panel); min-width: 0; }
	.card-head { display: flex; justify-content: space-between; align-items: center; gap: 16px; padding: 18px 20px 14px; border-bottom: 1px solid var(--rule); }
	h2 { font-size: 15px; font-weight: 650; letter-spacing: -0.2px; }
	.card-sub { font-size: 12px; color: var(--sub); margin-top: 2px; }
	.history { height: clamp(260px, 44vh, 520px); overflow-y: auto; overscroll-behavior: contain; padding: 18px 20px; display: flex; flex-direction: column; gap: 12px; scrollbar-width: thin; }
	.history:focus-visible { outline: 3px solid var(--accent-strong); outline-offset: -3px; }
	.empty { margin: auto 0; max-width: 46ch; }
	.empty h3 { font-size: 21px; font-weight: 650; letter-spacing: -0.4px; margin-bottom: 6px; }
	.empty p { font-size: 14px; line-height: 1.35; color: var(--sub); }
	.msg p { white-space: pre-wrap; overflow-wrap: anywhere; }
	.msg.user { align-self: flex-end; max-width: 80%; background: var(--accent-soft); color: var(--accent-strong); font-size: 14px; line-height: 1.4; padding: 7px 12px; border-radius: 12px 12px 4px 12px; }
	.msg.user time { display: block; text-align: right; font-size: 11px; opacity: 0.7; margin-top: 2px; }
	.msg.reply { display: flex; gap: 9px; align-items: flex-start; max-width: 92%; }
	.msg.reply .bar { width: 3px; align-self: stretch; border-radius: 2px; background: var(--accent); opacity: 0.6; flex-shrink: 0; }
	.msg.reply p { font-size: 14.5px; line-height: 1.45; }
	.msg.reply .who { display: flex; align-items: center; gap: 6px; font-size: 11px; font-weight: 700; letter-spacing: 0.6px; color: var(--sub); text-transform: uppercase; margin-bottom: 3px; }
	.who span { font-size: 10px; background: var(--chip-bg); color: var(--chip-ink); padding: 1px 6px; border-radius: 8px; letter-spacing: 0.4px; }
	.who time { font-weight: 500; letter-spacing: 0; }
	.composer { padding: 14px 20px 18px; border-top: 1px solid var(--rule); }
	.composer-label { display: flex; justify-content: space-between; align-items: baseline; gap: 12px; margin-bottom: 6px; }
	.composer-label label { font-size: 13px; font-weight: 550; }
	.composer-label span { font-size: 11px; color: var(--sub); font-variant-numeric: tabular-nums; }
	.composer-label span.over { color: #8a3b2c; font-weight: 600; }
	textarea { width: 100%; min-height: 84px; max-height: 320px; resize: vertical; background: #fff; border: 1px solid var(--rule-strong); border-radius: 12px; color: var(--ink); padding: 10px 12px; font: inherit; font-size: 14px; line-height: 1.45; }
	textarea:hover { border-color: var(--accent); }
	textarea[aria-invalid='true'] { border-color: #a65b48; }
	.composer-actions { display: flex; justify-content: space-between; align-items: center; gap: 12px; margin: 10px 0 6px; }
	.hint { font-size: 12px; color: var(--sub); line-height: 1.45; }
	.hint.state-label:empty { display: none; }
	.hint.warn { border-left: 3px solid var(--mic); padding-left: 10px; margin: 12px 0 16px; }
	.facts { padding: 18px 20px; }
	dl { margin: 12px 0 14px; font-size: 13px; }
	dl > div { display: flex; justify-content: space-between; gap: 12px; padding: 7px 0; border-bottom: 1px solid var(--rule); }
	dl > div:last-child { border-bottom: none; }
	dt { color: var(--sub); flex-shrink: 0; }
	dd { text-align: right; font-weight: 550; overflow-wrap: anywhere; min-width: 0; }
	.notice { margin: 12px 20px 0; }
    .archive { grid-column: 1 / -1; scroll-margin-top: 24px; }
    .archive-body { padding: 20px; display: grid; gap: 16px; }
    .archive-detail { display: grid; gap: 16px; min-width: 0; }
    .archive-detail h3 { font-size: 15px; }
    .conversation-list, .tool-list { list-style: none; padding: 0; display: grid; gap: 8px; }
    .conversation-list button { width: 100%; text-align: left; white-space: normal; overflow-wrap: anywhere; }
    .conversation-list small { display: block; margin-top: 4px; }
    .paging { display: flex; flex-wrap: wrap; gap: 8px; }
    .archive .a-btn { min-height: 44px; }
    .archive-filter { display: grid; gap: 8px; }
    .archive-filter label { font-size: 13px; font-weight: 600; }
    .archive-filter input, .archive-filter select { width: 100%; min-height: 44px; color: var(--ink); background: var(--paper); border: 1px solid var(--rule-strong); border-radius: 8px; padding: 8px 12px; font: inherit; }
    .archive-record { border-top: 1px solid var(--rule); padding-top: 16px; overflow-wrap: anywhere; }
    .archive-record > p { white-space: pre-wrap; line-height: 1.5; }
    .archive-record summary { cursor: pointer; padding: 8px 0; min-height: 44px; }
    .archive-record pre { white-space: pre-wrap; overflow-wrap: anywhere; font-size: 12px; line-height: 1.5; max-height: 360px; overflow: auto; }
    .tool-list li, .source-card { border: 1px solid var(--rule); border-radius: 8px; padding: 12px; margin-top: 8px; overflow-wrap: anywhere; font-size: 13px; line-height: 1.5; }
    .tool-list span, .tool-list small, .source-card small, .source-card a { display: block; }
    .tool-list .succeeded { border-left: 3px solid #628b7e; }
    .tool-list .attention { border-left: 3px solid #a65b48; }
    .source-card a { color: var(--accent-strong); text-decoration: underline; text-underline-offset: 3px; margin: 4px 0; }
    .source-card a:hover { text-decoration-thickness: 2px; }
    .home :is(button,a,input,select,textarea,summary):focus-visible { outline: 3px solid var(--accent-strong); outline-offset: 3px; }
    .home button:disabled { opacity: 0.55; cursor: not-allowed; }
    .archive button:hover:not(:disabled) { background: var(--accent-soft); }
    .archive button:active:not(:disabled) { transform: translateY(1px); }
    .archive .notice { margin: 0; }
	@media (max-width: 880px) {
		.home { grid-template-columns: minmax(0, 1fr); padding: 20px 16px; }
	}
	@media (max-width: 560px) {
		.case { padding: 0; background: none; box-shadow: none; }
		.bezel { padding: 0; background: none; box-shadow: none; border-radius: 12px; overflow: hidden; }
		.card-head, .composer, .history { padding-left: 14px; padding-right: 14px; }
		.composer-actions { flex-wrap: wrap; }
	}
</style>
