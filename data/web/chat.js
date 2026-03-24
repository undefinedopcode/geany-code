// Geany Code - Chat UI JavaScript
// Bridge uses custom URI scheme instead of Qt WebChannel

let messages = {};

// ── Bridge: JS → C communication via custom URI scheme ──────────

const bridge = {
    respondToPermission(requestId, optionId) {
        const url = `geanycode://permission-response?request_id=${encodeURIComponent(requestId)}&option_id=${encodeURIComponent(optionId)}`;
        fetch(url).catch(() => {});
    },

    jumpToEdit(filePath, startLine, endLine) {
        const url = `geanycode://jump-to-edit?file=${encodeURIComponent(filePath)}&start=${startLine}&end=${endLine}`;
        fetch(url).catch(() => {});
    },

    log(msg) {
        const url = `geanycode://log?${encodeURIComponent(msg)}`;
        fetch(url).catch(() => {});
    }
};

// ── Language detection for syntax highlighting ──────────────────

const extToLanguage = {
    'js': 'javascript', 'mjs': 'javascript', 'cjs': 'javascript',
    'ts': 'typescript', 'tsx': 'typescript', 'jsx': 'javascript',
    'html': 'xml', 'htm': 'xml',
    'css': 'css', 'scss': 'scss', 'less': 'less',
    'json': 'json',
    'c': 'c', 'h': 'c',
    'cpp': 'cpp', 'cxx': 'cpp', 'cc': 'cpp', 'hpp': 'cpp',
    'rs': 'rust', 'go': 'go', 'zig': 'zig',
    'java': 'java', 'kt': 'kotlin', 'scala': 'scala',
    'py': 'python', 'rb': 'ruby', 'php': 'php',
    'lua': 'lua',
    'sh': 'bash', 'bash': 'bash', 'zsh': 'bash',
    'yaml': 'yaml', 'yml': 'yaml',
    'toml': 'ini', 'ini': 'ini',
    'xml': 'xml', 'svg': 'xml',
    'md': 'markdown',
    'sql': 'sql',
    'cmake': 'cmake', 'makefile': 'makefile',
    'dockerfile': 'dockerfile',
    'swift': 'swift', 'cs': 'csharp',
    'ex': 'elixir', 'hs': 'haskell',
    'diff': 'diff', 'patch': 'diff',
};

function detectLanguage(info) {
    if (!info) return null;
    const lang = info.trim().toLowerCase();
    return extToLanguage[lang] || lang;
}

// ── Tool type detection ─────────────────────────────────────────

function isEditTool(name) {
    return name === 'Edit' || name === 'mcp__acp__Edit' ||
           (name && name.toLowerCase().includes('edit'));
}

function isWriteTool(name) {
    return name === 'Write' || name === 'mcp__acp__Write' ||
           (name && name.toLowerCase().includes('write'));
}

function isReadTool(name) {
    return name === 'Read' || name === 'mcp__acp__Read' ||
           (name && name.toLowerCase().includes('read'));
}

function isGlobTool(name) {
    return name === 'Glob' || name === 'mcp__acp__Glob' ||
           (name && name.toLowerCase().includes('glob'));
}

function isBashTool(name) {
    if (!name) return false;
    return name === 'Bash' || name === 'mcp__acp__Bash' ||
           name.toLowerCase().includes('bash');
}

function isGrepTool(name) {
    return name === 'Grep' || name === 'mcp__acp__Grep' ||
           (name && name.toLowerCase().includes('grep'));
}

// Parse "Exited with code X.Final output:\n\n<output>"
function parseBashResult(result) {
    if (!result) return { exitCode: null, output: '' };
    const match = result.match(/^Exited with code (\d+)\.Final output:\n?\n?([\s\S]*)$/);
    if (match) return { exitCode: parseInt(match[1], 10), output: match[2] || '' };
    return { exitCode: null, output: result };
}

// Strip non-SGR escape sequences (cursor movement, clearing, OSC, etc.)
function stripNonSgrEscapes(text) {
    // Remove OSC sequences: \x1b] ... \x07 or \x1b] ... \x1b\\
    text = text.replace(/\x1b\][^\x07]*(?:\x07|\x1b\\)/g, '');
    // Remove CSI sequences that are NOT SGR (SGR ends with 'm')
    // CSI = \x1b[ followed by params and a final byte in 0x40-0x7E range
    text = text.replace(/\x1b\[[0-9;?]*[A-LN-Za-ln-z]/g, '');
    // Remove other escape sequences (\x1b followed by single char)
    text = text.replace(/\x1b[()][0-9A-Za-z]/g, '');
    text = text.replace(/\x1b[78DMHc]/g, '');
    return text;
}

// Simple ANSI to HTML (colors only)
function ansiToHtml(text) {
    text = stripNonSgrEscapes(text);
    const fgColors = {
        '30': 'ansi-black', '31': 'ansi-red', '32': 'ansi-green',
        '33': 'ansi-yellow', '34': 'ansi-blue', '35': 'ansi-magenta',
        '36': 'ansi-cyan', '37': 'ansi-white',
        '90': 'ansi-bright-black', '91': 'ansi-bright-red',
        '92': 'ansi-bright-green', '93': 'ansi-bright-yellow',
        '94': 'ansi-bright-blue', '95': 'ansi-bright-magenta',
        '96': 'ansi-bright-cyan', '97': 'ansi-bright-white'
    };
    const bgColors = {
        '40': 'ansi-bg-black', '41': 'ansi-bg-red', '42': 'ansi-bg-green',
        '43': 'ansi-bg-yellow', '44': 'ansi-bg-blue', '45': 'ansi-bg-magenta',
        '46': 'ansi-bg-cyan', '47': 'ansi-bg-white'
    };

    let result = '';
    let classes = [];
    const regex = /\x1b\[([0-9;]*)m/g;
    let lastIndex = 0;
    let match;

    while ((match = regex.exec(text)) !== null) {
        if (match.index > lastIndex)
            result += escapeHtml(text.substring(lastIndex, match.index));
        lastIndex = regex.lastIndex;

        for (const code of match[1].split(';').filter(c => c !== '')) {
            if (code === '0' || code === '') {
                if (classes.length > 0) { result += '</span>'; classes = []; }
            } else if (code === '1') {
                if (classes.length > 0) result += '</span>';
                classes.push('ansi-bold');
                result += `<span class="${classes.join(' ')}">`;
            } else if (fgColors[code]) {
                if (classes.length > 0) result += '</span>';
                classes = classes.filter(c => !fgColors[Object.keys(fgColors).find(k => fgColors[k] === c)]);
                classes.push(fgColors[code]);
                result += `<span class="${classes.join(' ')}">`;
            } else if (bgColors[code]) {
                if (classes.length > 0) result += '</span>';
                classes = classes.filter(c => !c.startsWith('ansi-bg-'));
                classes.push(bgColors[code]);
                result += `<span class="${classes.join(' ')}">`;
            }
        }
    }

    if (lastIndex < text.length)
        result += escapeHtml(text.substring(lastIndex));
    if (classes.length > 0)
        result += '</span>';

    return result;
}

// Parse numbered line output: "     1→content" or "     1\tcontent"
function parseNumberedLines(text) {
    const lines = text.split('\n');
    const result = [];
    for (const line of lines) {
        // Match: optional spaces, digits, then → or \t, then content
        const match = line.match(/^\s*(\d+)[→\t](.*)$/);
        if (match) {
            result.push({ num: parseInt(match[1]), text: match[2] });
        } else if (line.trim() === '') {
            continue;
        } else {
            result.push({ num: 0, text: line });
        }
    }
    return result;
}

// Render syntax-highlighted code with line numbers
function renderReadResult(text, filePath) {
    const parsed = parseNumberedLines(text);
    if (parsed.length === 0) return escapeHtml(text);

    // Reconstruct just the code (without line numbers) for highlighting
    const codeText = parsed.map(l => l.text).join('\n');
    const highlighted = highlightLines(codeText, filePath || '');

    const lines = [];
    for (let i = 0; i < parsed.length; i++) {
        const num = parsed[i].num || (i + 1);
        const content = highlighted[i] || escapeHtml(parsed[i].text);
        lines.push(
            `<span class="read-line">` +
            `<span class="read-line-num">${num}</span>` +
            `<span class="read-line-content">${content}</span>` +
            `</span>`
        );
    }
    return lines.join('');
}

// ── LCS-based diff algorithm ────────────────────────────────────

function computeDiff(oldLines, newLines) {
    const n = oldLines.length, m = newLines.length;
    // Build LCS table
    const lcs = Array(n + 1).fill(null).map(() => Array(m + 1).fill(0));
    for (let i = 1; i <= n; i++) {
        for (let j = 1; j <= m; j++) {
            if (oldLines[i - 1] === newLines[j - 1])
                lcs[i][j] = lcs[i - 1][j - 1] + 1;
            else
                lcs[i][j] = Math.max(lcs[i - 1][j], lcs[i][j - 1]);
        }
    }
    // Backtrack
    const diff = [];
    let i = n, j = m;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && oldLines[i - 1] === newLines[j - 1]) {
            diff.unshift({ type: 'equal', value: oldLines[i - 1], oldIdx: i - 1, newIdx: j - 1 });
            i--; j--;
        } else if (j > 0 && (i === 0 || lcs[i][j - 1] >= lcs[i - 1][j])) {
            diff.unshift({ type: 'insert', value: newLines[j - 1], newIdx: j - 1 });
            j--;
        } else {
            diff.unshift({ type: 'delete', value: oldLines[i - 1], oldIdx: i - 1 });
            i--;
        }
    }
    return diff;
}

function escapeHtml(str) {
    return str.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

// Split highlight.js HTML output into per-line fragments, preserving span tags
function splitHighlightedLines(html, expectedCount) {
    const lines = [];
    let currentLine = '';
    let openTags = [];

    let i = 0;
    while (i < html.length) {
        const ch = html[i];
        if (ch === '\n') {
            let closing = '';
            for (let t = openTags.length - 1; t >= 0; t--) closing += '</span>';
            lines.push(currentLine + closing);
            currentLine = '';
            for (let t = 0; t < openTags.length; t++)
                currentLine += `<span class="${openTags[t]}">`;
            i++;
        } else if (ch === '<') {
            const tagEnd = html.indexOf('>', i);
            if (tagEnd === -1) { currentLine += ch; i++; continue; }
            const tagContent = html.substring(i + 1, tagEnd);
            const fullTag = html.substring(i, tagEnd + 1);
            if (tagContent.startsWith('/')) {
                openTags.pop();
                currentLine += fullTag;
            } else if (tagContent.startsWith('span')) {
                const m = tagContent.match(/class="([^"]+)"/);
                openTags.push(m ? m[1] : '');
                currentLine += fullTag;
            } else {
                currentLine += fullTag;
            }
            i = tagEnd + 1;
        } else {
            currentLine += ch;
            i++;
        }
    }
    if (currentLine || lines.length < expectedCount) {
        let closing = '';
        for (let t = openTags.length - 1; t >= 0; t--) closing += '</span>';
        lines.push(currentLine + closing);
    }
    while (lines.length < expectedCount) lines.push('');
    return lines;
}

// Highlight code and split into per-line HTML fragments
function highlightLines(text, fileName) {
    const lines = text.split('\n');
    const ext = fileName ? fileName.split('.').pop().toLowerCase() : '';
    const lang = detectLanguage(ext);

    if (typeof hljs === 'undefined' || !lang) {
        return lines.map(l => escapeHtml(l));
    }

    try {
        const highlighted = hljs.highlight(text, { language: lang }).value;
        return splitHighlightedLines(highlighted, lines.length);
    } catch (e) {
        return lines.map(l => escapeHtml(l));
    }
}

function generateUnifiedDiff(oldText, newText, fileName) {
    const oldLines = oldText.split('\n');
    const newLines = newText.split('\n');
    const diff = computeDiff(oldLines, newLines);
    const contextSize = 3;

    // Syntax-highlight both sides
    const highlightedOld = highlightLines(oldText, fileName);
    const highlightedNew = highlightLines(newText, fileName);

    const lines = [];
    if (fileName) {
        lines.push(`<span class="diff-header">--- ${escapeHtml(fileName)}</span>`);
        lines.push(`<span class="diff-header">+++ ${escapeHtml(fileName)}</span>`);
    }

    // Find change regions and render with context
    let idx = 0;
    while (idx < diff.length) {
        while (idx < diff.length && diff[idx].type === 'equal') idx++;
        if (idx >= diff.length) break;

        const hunkStart = Math.max(0, idx - contextSize);
        let end = idx;
        while (end < diff.length) {
            if (diff[end].type !== 'equal') { end++; continue; }
            let nextChange = end;
            while (nextChange < diff.length && diff[nextChange].type === 'equal') nextChange++;
            if (nextChange < diff.length && nextChange - end <= contextSize * 2) {
                end = nextChange + 1;
            } else {
                break;
            }
        }
        const hunkEnd = Math.min(diff.length, end + contextSize);

        if (lines.length > 2) lines.push(`<span class="diff-separator">···</span>`);

        for (let k = hunkStart; k < hunkEnd; k++) {
            const d = diff[k];
            if (d.type === 'equal') {
                const content = highlightedOld[d.oldIdx] || escapeHtml(d.value);
                lines.push(`<span class="diff-context"> ${content}</span>`);
            } else if (d.type === 'delete') {
                const content = highlightedOld[d.oldIdx] || escapeHtml(d.value);
                lines.push(`<span class="diff-remove">-${content}</span>`);
            } else if (d.type === 'insert') {
                const content = highlightedNew[d.newIdx] || escapeHtml(d.value);
                lines.push(`<span class="diff-add">+${content}</span>`);
            }
        }

        idx = hunkEnd;
    }

    if (lines.length <= 2) {
        lines.push(`<span class="diff-context"> (no changes)</span>`);
    }

    return lines.join('');
}

// ── Markdown configuration ──────────────────────────────────────

function configureMarked() {
    if (typeof marked === 'undefined') return;

    const renderer = new marked.Renderer();

    // Custom code block rendering with header and copy button
    renderer.code = function(code, language) {
        // Handle marked v4+ object argument
        if (typeof code === 'object') {
            language = code.lang;
            code = code.text;
        }
        const lang = detectLanguage(language);
        let highlighted = code;

        if (typeof hljs !== 'undefined' && lang) {
            try {
                highlighted = hljs.highlight(code, { language: lang }).value;
            } catch (e) {
                highlighted = hljs.highlightAuto(code).value;
            }
        } else if (typeof hljs !== 'undefined') {
            highlighted = hljs.highlightAuto(code).value;
        }

        const langLabel = language || '';
        return `<div class="code-block">
            <div class="code-header">
                <span>${langLabel}</span>
                <button class="copy-btn" onclick="copyCode(this)">Copy</button>
            </div>
            <pre><code class="hljs ${lang || ''}">${highlighted}</code></pre>
        </div>`;
    };

    marked.setOptions({
        renderer: renderer,
        breaks: true,
        gfm: true,
    });
}

// ── Copy code to clipboard ──────────────────────────────────────

function copyCode(btn) {
    const codeBlock = btn.closest('.code-block');
    const code = codeBlock.querySelector('code').textContent;

    navigator.clipboard.writeText(code).then(() => {
        btn.textContent = 'Copied!';
        setTimeout(() => { btn.textContent = 'Copy'; }, 1500);
    });
}

// ── Render markdown content ─────────────────────────────────────

function renderMarkdown(text) {
    if (typeof marked !== 'undefined') {
        return marked.parse(text);
    }
    // Fallback: basic escaping
    return text
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/\n/g, '<br>');
}

// ── User question UI (MCP ask_user tool) ────────────────────────

function showUserQuestion(requestId, questionsJson) {
    let questions;
    try {
        questions = JSON.parse(questionsJson);
    } catch (e) {
        return;
    }

    const container = document.getElementById('messages');
    const el = document.createElement('div');
    el.className = 'user-question';
    el.id = `question-${requestId}`;

    let html = '<div class="uq-title">Claude is asking:</div>';

    for (const q of questions) {
        const header = q.header || '';
        const question = q.question || '';
        const multi = q.multiSelect || false;
        const options = q.options || [];
        const inputType = multi ? 'checkbox' : 'radio';

        html += `<div class="uq-question" data-header="${escapeHtml(header)}">`;
        html += `<div class="uq-question-text">${escapeHtml(question)}</div>`;
        html += `<div class="uq-options">`;

        for (const opt of options) {
            const label = opt.label || '';
            const desc = opt.description || '';
            html += `<label class="uq-option">
                <input type="${inputType}" name="uq_${escapeHtml(header)}"
                       value="${escapeHtml(label)}">
                <span class="uq-opt-label">${escapeHtml(label)}</span>
                <span class="uq-opt-desc">${escapeHtml(desc)}</span>
            </label>`;
        }

        html += `</div></div>`;
    }

    html += `<div class="uq-actions">
        <button class="uq-submit" onclick="submitUserQuestion('${escapeHtml(requestId)}')">Submit</button>
        <button class="uq-cancel" onclick="cancelUserQuestion('${escapeHtml(requestId)}')">Cancel</button>
    </div>`;

    el.innerHTML = html;
    container.appendChild(el);
    scrollToBottom();
}

function submitUserQuestion(requestId) {
    const el = document.getElementById(`question-${requestId}`);
    if (!el) return;

    // Collect answers keyed by header
    const answers = {};
    const questions = el.querySelectorAll('.uq-question');
    for (const q of questions) {
        const header = q.dataset.header;
        const checked = q.querySelectorAll('input:checked');
        answers[header] = Array.from(checked).map(c => c.value);
    }

    const responseJson = JSON.stringify(answers);
    const url = `geanycode://question-response?request_id=${encodeURIComponent(requestId)}&response=${encodeURIComponent(responseJson)}`;
    fetch(url).catch(() => {});

    // Replace with a summary
    el.innerHTML = `<div class="uq-answered">Answered: ${Object.entries(answers).map(([k,v]) => `${k}: ${v.join(', ')}`).join(' | ')}</div>`;
}

function cancelUserQuestion(requestId) {
    const url = `geanycode://question-response?request_id=${encodeURIComponent(requestId)}&response=${encodeURIComponent('ERROR: User cancelled')}`;
    fetch(url).catch(() => {});

    const el = document.getElementById(`question-${requestId}`);
    if (el) el.innerHTML = '<div class="uq-answered">Cancelled</div>';
}

// ── Todo tracking ───────────────────────────────────────────────

function updateTodos(todosJson) {
    let todos;
    try {
        todos = JSON.parse(todosJson);
    } catch (e) {
        return;
    }

    const container = document.getElementById('todos-container');
    if (!container) return;

    if (!todos || todos.length === 0) {
        container.innerHTML = '';
        container.style.display = 'none';
        document.body.style.paddingBottom = '0';
        return;
    }

    container.style.display = 'block';

    const completedCount = todos.filter(t => t.status === 'completed').length;
    const totalCount = todos.length;
    const isCollapsed = container.dataset.collapsed === 'true';

    let html = `
        <div class="todos-header" onclick="toggleTodos()">
            <span class="todos-title">Tasks (${completedCount}/${totalCount})</span>
            <span class="todos-toggle">${isCollapsed ? '▶' : '▼'}</span>
        </div>
        <div class="todos-content ${isCollapsed ? 'collapsed' : ''}">
            <div class="todos-list">
    `;

    for (const todo of todos) {
        const status = todo.status || 'pending';
        const content = todo.content || '';

        let icon;
        if (status === 'completed') icon = '✓';
        else if (status === 'in_progress') icon = '⟳';
        else icon = '○';

        html += `
            <div class="todo-item ${status}">
                <span class="todo-icon">${icon}</span>
                <span class="todo-text">${escapeHtml(content)}</span>
            </div>
        `;
    }

    html += '</div></div>';
    container.innerHTML = html;

    // Add bottom padding to body so messages aren't hidden behind the fixed panel
    requestAnimationFrame(() => {
        document.body.style.paddingBottom = container.offsetHeight + 'px';
        scrollToBottom();
    });
}

function toggleTodos() {
    const container = document.getElementById('todos-container');
    if (!container) return;

    const content = container.querySelector('.todos-content');
    const toggle = container.querySelector('.todos-toggle');
    if (!content || !toggle) return;

    const isCollapsed = content.classList.toggle('collapsed');
    toggle.textContent = isCollapsed ? '▶' : '▼';
    container.dataset.collapsed = isCollapsed;
}

// ── Scroll to bottom ────────────────────────────────────────────

function scrollToBottom() {
    window.scrollTo({ top: document.body.scrollHeight, behavior: 'smooth' });
}

// ── Public API (called from C via evaluate_javascript) ──────────

function addMessage(id, role, content, timestamp, isStreaming, images) {
    const container = document.getElementById('messages');
    const welcome = document.getElementById('welcome-screen');

    // Hide welcome screen on first message
    if (welcome) welcome.style.display = 'none';

    // Create message element
    const el = document.createElement('div');
    el.className = `message ${role}`;
    el.id = `msg-${id}`;

    const contentDiv = document.createElement('div');
    contentDiv.className = 'content';

    if (role === 'user') {
        contentDiv.innerHTML = renderMarkdown(content);
    } else {
        contentDiv.innerHTML = renderMarkdown(content);
        if (isStreaming) contentDiv.classList.add('streaming-cursor');
    }

    el.appendChild(contentDiv);
    container.appendChild(el);

    messages[id] = { role, el, contentDiv };
    scrollToBottom();
}

function addMessageImage(msgId, b64Png) {
    const msg = messages[msgId];
    if (!msg) return;

    const img = document.createElement('img');
    img.src = 'data:image/png;base64,' + b64Png;
    img.className = 'message-image';
    msg.el.appendChild(img);
    scrollToBottom();
}

function updateMessage(id, content, isStreaming) {
    const msg = messages[id];
    if (!msg) return;

    if (msg.role === 'user') {
        msg.contentDiv.innerHTML = renderMarkdown(content);
    } else {
        msg.contentDiv.innerHTML = renderMarkdown(content);
        if (isStreaming) {
            msg.contentDiv.classList.add('streaming-cursor');
        } else {
            msg.contentDiv.classList.remove('streaming-cursor');
        }
    }

    scrollToBottom();
}

function addToolCall(msgId, toolId, toolName, inputJson, result) {
    // If this toolId already exists, this is a result update
    const existing = document.getElementById(`tool-${toolId}`);
    if (existing) {
        updateToolResult(toolId, result || '', toolName);
        return;
    }

    const container = document.getElementById('messages');
    let input = null;
    try { input = inputJson ? JSON.parse(inputJson) : null; } catch (e) {}

    const el = document.createElement('div');
    el.className = 'tool-call';
    el.id = `tool-${toolId}`;

    // Determine display name and file path
    let displayName = toolName;
    let filePath = input ? (input.file_path || input.path || '') : '';
    const shortPath = filePath ? filePath.split('/').slice(-2).join('/') : '';

    let slugLabel = '';
    if (toolName === 'TodoWrite') displayName = 'Todo';
    else if (isEditTool(toolName)) displayName = 'Edit';
    else if (isWriteTool(toolName)) displayName = 'Write';
    else if (isReadTool(toolName)) displayName = 'Read';
    else if (isGlobTool(toolName)) {
        displayName = 'Glob';
        slugLabel = input ? (input.pattern || '') : '';
    } else if (isGrepTool(toolName)) {
        displayName = 'Grep';
        slugLabel = input ? (input.pattern || '') : '';
    } else if (isBashTool(toolName)) {
        displayName = 'Bash';
        if (input && input.command) {
            const cmd = input.command;
            slugLabel = cmd.length > 60 ? cmd.substring(0, 60) + '...' : cmd;
        }
    }

    el.dataset.toolType = displayName;
    el.dataset.filePath = filePath;

    const header = document.createElement('div');
    header.className = 'tool-call-header';
    const slug = slugLabel || shortPath;
    const fileLabel = slug ? ` <span class="tool-call-file">${escapeHtml(slug)}</span>` : '';
    header.innerHTML = `<span class="tool-call-arrow">▶</span> <span class="tool-call-name">${escapeHtml(displayName)}</span>${fileLabel} <span class="tool-call-status">running</span>`;

    const body = document.createElement('div');
    body.className = 'tool-call-body';

    // Collapsible parameters section for tools with rich rendering
    function addParamsSection(parent, inputJson) {
        if (!inputJson) return;
        let formatted;
        try { formatted = JSON.stringify(JSON.parse(inputJson), null, 2); }
        catch (e) { formatted = inputJson; }

        const details = document.createElement('details');
        details.className = 'tool-params';
        const summary = document.createElement('summary');
        summary.textContent = 'Parameters';
        details.appendChild(summary);
        const pre = document.createElement('pre');
        pre.textContent = formatted;
        details.appendChild(pre);
        parent.appendChild(details);
    }

    // Render diff for Edit tools, file content for Write tools, JSON for others
    const oldStr = input ? (input.old_string ?? input.old_text ?? undefined) : undefined;
    const newStr = input ? (input.new_string ?? input.new_text ?? '') : '';

    if (isEditTool(toolName) && input && oldStr !== undefined) {
        const diffHtml = generateUnifiedDiff(
            oldStr || '',
            newStr || '',
            filePath
        );
        body.innerHTML = `<pre class="diff">${diffHtml}</pre>`;

        // Add jump-to-edit link if we have a file path
        if (filePath) {
            const jumpLink = document.createElement('div');
            jumpLink.className = 'tool-call-jump';
            jumpLink.textContent = 'Jump to file';
            jumpLink.addEventListener('click', (e) => {
                e.stopPropagation();
                bridge.jumpToEdit(filePath, 1, 1);
            });
            body.appendChild(jumpLink);
        }

        addParamsSection(body, inputJson);

        // Auto-expand edit diffs
        body.classList.add('expanded');
        header.querySelector('.tool-call-arrow').textContent = '▼';

    } else if (isWriteTool(toolName) && input && input.content !== undefined) {
        // Show file content with language detection
        const lang = detectLanguage(filePath ? filePath.split('.').pop() : '');
        let highlighted = escapeHtml(input.content);
        if (typeof hljs !== 'undefined' && lang) {
            try {
                highlighted = hljs.highlight(input.content, { language: lang }).value;
            } catch (e) {}
        }
        body.innerHTML = `<div class="tool-write-header">${escapeHtml(filePath || 'new file')}</div><pre class="tool-write-content"><code>${highlighted}</code></pre>`;

        addParamsSection(body, inputJson);

        // Auto-expand writes
        body.classList.add('expanded');
        header.querySelector('.tool-call-arrow').textContent = '▼';

    } else if (isBashTool(toolName)) {
        // Bash: show full command, result rendered by updateToolResult
        if (input && input.command) {
            const cmdPre = document.createElement('pre');
            cmdPre.className = 'bash-command';
            cmdPre.textContent = input.command;
            body.appendChild(cmdPre);
        }
        addParamsSection(body, inputJson);

        body.classList.add('expanded');
        header.querySelector('.tool-call-arrow').textContent = '▼';

    } else if (isReadTool(toolName) || isGlobTool(toolName) || isGrepTool(toolName)) {
        // Read/Glob/Grep: result will be rendered by updateToolResult
        addParamsSection(body, inputJson);

        body.classList.add('expanded');
        header.querySelector('.tool-call-arrow').textContent = '▼';

    } else {
        // Generic tool: show JSON
        let bodyContent = '';
        if (inputJson) {
            try {
                bodyContent = JSON.stringify(JSON.parse(inputJson), null, 2);
            } catch (e) {
                bodyContent = inputJson;
            }
        }
        if (result) {
            bodyContent += '\n\n── Result ──\n' + result;
        }
        body.textContent = bodyContent;
    }

    header.addEventListener('click', () => {
        body.classList.toggle('expanded');
        header.querySelector('.tool-call-arrow').textContent =
            body.classList.contains('expanded') ? '▼' : '▶';
    });

    el.appendChild(header);
    el.appendChild(body);
    container.appendChild(el);

    scrollToBottom();
}

function updateToolResult(toolId, result, toolName) {
    const el = document.getElementById(`tool-${toolId}`);
    if (!el) return;  // No matching tool call element — drop silently

    // Update status indicator
    const status = el.querySelector('.tool-call-status');
    if (status) {
        const isError = toolName === '(error)';
        status.textContent = isError ? 'failed' : 'done';
        status.className = `tool-call-status ${isError ? 'error' : 'success'}`;
    }

    // Append result below existing content (preserving diff HTML)
    const body = el.querySelector('.tool-call-body');
    if (body && result) {
        const toolType = el.dataset.toolType || '';
        const filePath = el.dataset.filePath || '';

        if (toolType === 'Glob') {
            // Glob: render as a file list
            const files = result.split('\n').filter(f => f.trim());
            const listDiv = document.createElement('div');
            listDiv.className = 'glob-output';
            const countLabel = document.createElement('div');
            countLabel.className = 'glob-count';
            countLabel.textContent = `${files.length} file${files.length !== 1 ? 's' : ''}`;
            listDiv.appendChild(countLabel);

            const list = document.createElement('div');
            list.className = 'glob-list';
            for (const file of files) {
                const item = document.createElement('div');
                item.className = 'glob-item';
                item.textContent = file.trim();
                list.appendChild(item);
            }
            listDiv.appendChild(list);
            body.appendChild(listDiv);

            if (!body.classList.contains('expanded')) {
                body.classList.add('expanded');
                const arrow = el.querySelector('.tool-call-arrow');
                if (arrow) arrow.textContent = '▼';
            }
        } else if (toolType === 'Bash') {
            // Bash: parse exit code and render with ANSI colors
            const parsed = parseBashResult(result);
            const isError = parsed.exitCode !== null && parsed.exitCode !== 0;

            const section = document.createElement('div');
            section.className = `bash-result-section ${isError ? 'exit-error' : 'exit-success'}`;

            if (parsed.exitCode !== null) {
                const exitDiv = document.createElement('div');
                exitDiv.className = `bash-exit-code ${isError ? 'exit-error' : 'exit-success'}`;
                exitDiv.innerHTML = `<strong>Exit code:</strong> <span class="exit-code-value">${parsed.exitCode}</span>`;
                section.appendChild(exitDiv);
            }

            if (parsed.output) {
                const outputDiv = document.createElement('div');
                outputDiv.className = 'bash-output';
                const pre = document.createElement('pre');
                pre.innerHTML = ansiToHtml(parsed.output);
                outputDiv.appendChild(pre);
                section.appendChild(outputDiv);
            } else {
                const noOutput = document.createElement('div');
                noOutput.className = 'bash-no-output';
                noOutput.textContent = '(no output)';
                section.appendChild(noOutput);
            }

            body.appendChild(section);

        } else if (toolType === 'Read' && result.match(/^\s*\d+[→\t]/)) {
            // Read tool: render with syntax highlighting and line numbers
            const readHtml = renderReadResult(result, filePath);
            const readDiv = document.createElement('pre');
            readDiv.className = 'read-output';
            readDiv.innerHTML = readHtml;
            body.appendChild(readDiv);

            // Auto-expand read results
            if (!body.classList.contains('expanded')) {
                body.classList.add('expanded');
                const arrow = el.querySelector('.tool-call-arrow');
                if (arrow) arrow.textContent = '▼';
            }
        } else {
            const displayResult = result.length > 5000
                ? result.substring(0, 5000) + '\n... (truncated)'
                : result;
            const resultDiv = document.createElement('div');
            resultDiv.className = 'tool-call-result';
            resultDiv.textContent = displayResult;
            body.appendChild(resultDiv);
        }
    }

    scrollToBottom();
}

function showPermissionRequest(requestId, toolName, description, optionsJson) {
    const container = document.getElementById('messages');

    const el = document.createElement('div');
    el.className = 'permission-request';

    el.innerHTML = `
        <div class="title">Permission: ${toolName}</div>
        <div class="description">${description}</div>
        <div class="actions"></div>
    `;

    const actions = el.querySelector('.actions');

    let options;
    try {
        options = JSON.parse(optionsJson);
    } catch (e) {
        options = [
            { id: 'allow', label: 'Allow' },
            { id: 'deny', label: 'Deny' },
        ];
    }

    for (const opt of options) {
        const btn = document.createElement('button');
        btn.textContent = opt.label || opt.id;
        btn.addEventListener('click', () => {
            bridge.respondToPermission(requestId, opt.id);
            el.remove();
        });
        actions.appendChild(btn);
    }

    container.appendChild(el);
    scrollToBottom();
}

// ── Initialize ──────────────────────────────────────────────────

document.addEventListener('DOMContentLoaded', () => {
    configureMarked();
    bridge.log('Geany Code chat UI initialized');
});
