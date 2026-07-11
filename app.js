var API = '/api';

function showMsg(el, text, cls) {
    el.textContent = text;
    el.className = 'msg msg-' + cls;
    el.style.display = 'block';
    setTimeout(function() { el.style.display = 'none'; }, 3000);
}

function createFile() {
    var fpath = document.getElementById('ffpath').value.trim();
    var size = document.getElementById('fsize').value;
    var desc = document.getElementById('fdesc').value.trim();
    var fmsg = document.getElementById('fmsg');
    if (!fpath) { showMsg(fmsg, 'File path required', 'error'); return; }
    var body = JSON.stringify({file_path: fpath, size: parseInt(size), description: desc});
    fetch(API + '/files', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: body})
    .then(function(r) { return r.json(); }).then(function(j) {
        if (j.file_path) { showMsg(fmsg, 'Created: ' + j.file_path, 'success'); loadFiles(); }
        else { showMsg(fmsg, j.error, 'error'); }
    });
}

function loadFiles() {
    fetch(API + '/files').then(function(r) { return r.json(); }).then(function(files) {
        var h = '<table><tr><th>Path</th><th>Size</th><th>Description</th><th>Actions</th></tr>';
        for (var i = 0; i < files.length; i++) {
            var f = files[i];
            h += '<tr><td><span class="monospace">' + esc(f.file_path) + '</span></td>';
            h += '<td>' + fmtSize(f.size) + '</td>';
            h += '<td>' + esc(f.description || '') + '</td>';
            h += '<td><button class="btn btn-small btn-primary" onclick="loadBlocks(\'' + esc(f.file_path) + '\')">Blocks</button> ';
            h += '<button class="btn btn-small btn-danger" onclick="delFile(\'' + esc(f.file_path) + '\')">Del</button></td></tr>';
        }
        h += '</table>';
        document.getElementById('filelist').innerHTML = h;
    });
}

function delFile(path) {
    if (!confirm('Delete ' + path + ' and all its blocks?')) return;
    fetch(API + '/files?path=' + encodeURIComponent(path), {method: 'DELETE'}).then(loadFiles);
}

function createBlock() {
    var bpath = document.getElementById('bpath').value.trim();
    var sha = document.getElementById('bsha').value.trim();
    var size = document.getElementById('bsize').value;
    var bmsg = document.getElementById('bmsg');
    if (!bpath || !sha) { showMsg(bmsg, 'Block path and SHA-256 required', 'error'); return; }
    var body = JSON.stringify({block_path: bpath, sha256: sha, block_size: parseInt(size)});
    fetch(API + '/blocks', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: body})
    .then(function(r) { return r.json(); }).then(function(j) {
        if (typeof j.id !== 'undefined') { showMsg(bmsg, 'Created block ID: ' + j.id, 'success'); loadBlocks(); }
        else { showMsg(bmsg, j.error, 'error'); }
    });
    document.getElementById('bpath').value = '';
    document.getElementById('bsha').value = '';
}

function loadBlocks(fp) {
    if (fp) document.getElementById('blockfilter').value = fp;
    var path = document.getElementById('blockfilter').value.trim();
    fetch(API + '/files/blocks?path=' + encodeURIComponent(path || '/'))
    .then(function(r) { return r.json(); }).then(function(blocks) {
        var h = '<table><tr><th>ID</th><th>Path</th><th>Size</th><th>Status</th><th>SHA-256</th><th>Actions</th></tr>';
        for (var i = 0; i < blocks.length; i++) {
            var b = blocks[i];
            h += '<tr><td>' + b.id + '</td>';
            h += '<td><span class="monospace">' + esc(b.block_path) + '</span></td>';
            h += '<td>' + fmtSize(b.block_size) + '</td>';
            h += '<td>' + (b.is_bad ? '<span class="badge badge-bad">BAD</span>' : '<span class="badge badge-ok">OK</span>') + '</td>';
            h += '<td><span class="monospace" title="' + esc(b.sha256) + '">' + esc(b.sha256).slice(0, 16) + '...</span></td>';
            h += '<td><button class="btn btn-small btn-danger" onclick="delBlock(' + b.id + ')">Del</button> ';
            h += '<button class="btn btn-small btn-warning" onclick="markBad(' + b.id + ')">Bad</button></td></tr>';
        }
        h += '</table>';
        document.getElementById('blocklist').innerHTML = h;
    });
}

function appendBlock() {
    var fp = document.getElementById('atarget').value.trim();
    var bid = document.getElementById('ablock').value;
    fetch(API + '/files/blocks?path=' + encodeURIComponent(fp) + '&block=' + bid, {method: 'POST'})
    .then(function(r) {
        if (r.ok) { loadBlocks(fp); loadFiles(); }
        else { r.json().then(function(j) { alert(j.error); }); }
    });
}

function delBlock(id) {
    if (!confirm('Delete block #' + id + '?')) return;
    fetch(API + '/blocks/delete?block=' + id, {method: 'DELETE'}).then(loadBlocks);
}

function markBad(id) {
    fetch(API + '/blocks/bad?block=' + id, {method: 'PATCH'}).then(loadBlocks);
}

function esc(s) {
    var d = document.createElement('div');
    d.appendChild(document.createTextNode(s));
    return d.innerHTML;
}

function fmtSize(n) {
    if (n >= 1073741824) return (n / 1073741824).toFixed(2) + ' GB';
    if (n >= 1048576) return (n / 1048576).toFixed(2) + ' MB';
    if (n >= 1024) return (n / 1024).toFixed(2) + ' KB';
    return n + ' B';
}

loadFiles();
loadBlocks();
