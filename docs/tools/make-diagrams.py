#!/usr/bin/env python3
"""Append use-case, sequence and deployment diagrams to DataBackup.mdj.

The layout follows the hand-written style of the existing class diagram
(readable ids, minimal fields).  Element schemas that the class diagram does
not use yet (frame/lifeline/message/actor/node/component views) follow the
official StarUML sample model UMLExample.mdj.

Usage: gen.py [in.mdj] [out.mdj] [mermaid-dir]
"""
import json
import os
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else 'DataBackup.mdj'
OUT = sys.argv[2] if len(sys.argv) > 2 else 'DataBackup.new.mdj'
MERMAID_DIR = sys.argv[3] if len(sys.argv) > 3 else 'mermaid'

project = json.load(open(SRC, encoding='utf-8'))
COURSE_MODEL = 'backup-studio-course-model'
MERMAID = {}


def label(lid, parent, text, left, top, visible=True, width=140, height=14,
          align=2, font='Arial;13;0'):
    return {"_type": "LabelView", "_id": lid, "_parent": {"$ref": parent},
            "visible": visible, "font": font, "parentStyle": True, "left": left,
            "top": top, "width": width, "height": height, "autoResize": False,
            "underline": False, "text": text, "horizontalAlignment": align,
            "verticalAlignment": 5}


def name_compartment(cid, view_id, model, text, left, top, width, height=28):
    labels = [
        label(cid + '-stereotype', cid, '', left + 5, top + 7, False, width - 10),
        label(cid + '-name', cid, text, left + 5, top + 7, True, width - 10, 14, 2, 'Arial;13;1'),
        label(cid + '-namespace', cid, '', left + 5, top + 21, False, width - 10, 14, 1),
        label(cid + '-property', cid, '', left + 5, top + 21, False, width - 10, 14, 1),
    ]
    return {"_type": "UMLNameCompartmentView", "_id": cid, "_parent": {"$ref": view_id},
            "model": {"$ref": model}, "subViews": labels, "visible": True,
            "font": "Arial;13;0", "parentStyle": True, "left": left, "top": top,
            "width": width, "height": height, "autoResize": False,
            "stereotypeLabel": {"$ref": labels[0]['_id']},
            "nameLabel": {"$ref": labels[1]['_id']},
            "namespaceLabel": {"$ref": labels[2]['_id']},
            "propertyLabel": {"$ref": labels[3]['_id']}}


def hidden_compartment(vid, view_id, model, ctype, left, top):
    return {"_type": ctype, "_id": vid, "_parent": {"$ref": view_id},
            "model": {"$ref": model}, "visible": False, "font": "Arial;13;0",
            "left": left, "top": top, "width": 10, "height": 10}


def edge_label(lid, view_id, model, text, visible, distance):
    return {"_type": "EdgeLabelView", "_id": lid, "_parent": {"$ref": view_id},
            "model": {"$ref": model}, "visible": visible, "font": "Arial;13;0",
            "hostEdge": {"$ref": view_id}, "edgePosition": 1,
            "alpha": 1.5707963267948966, "distance": distance, "height": 14,
            "text": text}


def edge_refs(view_id, model, name_text, stereo_text=''):
    nm = edge_label(view_id + '-name', view_id, model, name_text, True, 15)
    st = edge_label(view_id + '-stereotype', view_id, model, stereo_text, bool(stereo_text), 30)
    pr = edge_label(view_id + '-property', view_id, model, '', False, 30)
    return [nm, st, pr], {"nameLabel": {"$ref": nm['_id']},
                          "stereotypeLabel": {"$ref": st['_id']},
                          "propertyLabel": {"$ref": pr['_id']}}


COMPARTMENT_KEYS = {
    'UMLAttributeCompartmentView': 'attributeCompartment',
    'UMLOperationCompartmentView': 'operationCompartment',
    'UMLReceptionCompartmentView': 'receptionCompartment',
    'UMLTemplateParameterCompartmentView': 'templateParameterCompartment',
    'UMLExtensionPointCompartmentView': 'extensionPointCompartment',
}


def node_view(vid, diagram_id, model, vtype, left, top, width, height, text,
              extra_compartments=(), contained=()):
    nc = name_compartment(vid + '-namecomp', vid, model, text, left, top, width)
    sub = [nc]
    v = {"_type": vtype, "_id": vid, "_parent": {"$ref": diagram_id},
         "model": {"$ref": model}, "subViews": sub, "font": "Arial;13;0",
         "containerChangeable": True, "left": left, "top": top,
         "width": width, "height": height, "nameCompartment": {"$ref": nc['_id']},
         "wordWrap": True, "suppressAttributes": True, "suppressOperations": True}
    for ctype in ("UMLAttributeCompartmentView", "UMLOperationCompartmentView",
                  "UMLReceptionCompartmentView", "UMLTemplateParameterCompartmentView"):
        cid = vid + '-' + ctype.lower()
        sub.append(hidden_compartment(cid, vid, model, ctype, left + 5, top + 30))
        v[COMPARTMENT_KEYS[ctype]] = {"$ref": cid}
    for ctype in extra_compartments:
        cid = vid + '-' + ctype.lower()
        sub.append(hidden_compartment(cid, vid, model, ctype, left + 5, top + 44))
        v[COMPARTMENT_KEYS[ctype]] = {"$ref": cid}
    if contained:
        v['containedViews'] = [{"$ref": c} for c in contained]
    return v


def edge_view(vid, diagram_id, model, vtype, tail_view, head_view, points,
              name_text='', stereo_text='', line_style=1):
    labels, refs = edge_refs(vid, model, name_text, stereo_text)
    v = {"_type": vtype, "_id": vid, "_parent": {"$ref": diagram_id},
         "model": {"$ref": model}, "subViews": labels, "font": "Arial;13;0",
         "head": {"$ref": head_view}, "tail": {"$ref": tail_view},
         "lineStyle": line_style, "points": points, "showVisibility": True}
    v.update(refs)
    return v

# ----------------------------------------------------------- use case diagram
UC_DIAGRAM = 'backup-studio-use-cases'
ACTORS = [("user", "用户（CLI / 桌面端）"), ("admin", "服务器管理员")]
USE_CASES = [
    ("local-backup", "本地备份"), ("automatic-backup", "自动备份（定时/实时）"),
    ("select-entries", "筛选与选择备份条目"), ("inspect", "查看归档信息"),
    ("local-restore", "本地还原"), ("register", "注册账户"), ("login", "登录服务器"),
    ("upload", "上传远程备份"), ("list", "查看远程备份列表"), ("remote-restore", "远程还原"),
    ("delete-account", "注销账户"), ("run-server", "运行与配置服务器"),
]
INCLUDES = [("automatic-backup", "local-backup"), ("local-backup", "select-entries"),
            ("upload", "local-backup"), ("upload", "login"),
            ("remote-restore", "login"), ("remote-restore", "local-restore"),
            ("list", "login"), ("delete-account", "login")]

uc_elements, uc_views = [], []
uc_elements.append({"_type": "UMLUseCaseSubject", "_id": "uc-subject",
                    "_parent": {"$ref": COURSE_MODEL}, "name": "Backup Studio 备份系统"})
for aid, aname in ACTORS:
    uc_elements.append({"_type": "UMLActor", "_id": "uc-actor-" + aid,
                        "_parent": {"$ref": COURSE_MODEL}, "name": aname})
for uid, uname in USE_CASES:
    uc_elements.append({"_type": "UMLUseCase", "_id": "uc-use-" + uid,
                        "_parent": {"$ref": COURSE_MODEL}, "name": uname})

SUBJECT_LEFT, SUBJECT_TOP, SUBJECT_W = 320, 60, 1140
SUBJECT_H = 60 + ((len(USE_CASES) + 2) // 3) * 100
uc_pos = {}
for i, (uid, uname) in enumerate(USE_CASES):
    col, row = i % 3, i // 3
    uc_pos[uid] = (SUBJECT_LEFT + 40 + col * 370, SUBJECT_TOP + 60 + row * 100)
ACTOR_POS = {"user": (40, 160), "admin": (40, 520)}

uc_views.append(node_view('uc-view-subject', UC_DIAGRAM, 'uc-subject',
                          'UMLUseCaseSubjectView', SUBJECT_LEFT, SUBJECT_TOP,
                          SUBJECT_W, SUBJECT_H, "Backup Studio 备份系统"))
for aid, aname in ACTORS:
    x, y = ACTOR_POS[aid]
    uc_views.append(node_view('uc-view-actor-' + aid, UC_DIAGRAM, 'uc-actor-' + aid,
                              'UMLActorView', x, y, 200, 70, aname))
for uid, uname in USE_CASES:
    x, y = uc_pos[uid]
    extra = () if uid == 'run-server' else ('UMLExtensionPointCompartmentView',)
    uc_views.append(node_view('uc-view-use-' + uid, UC_DIAGRAM, 'uc-use-' + uid,
                              'UMLUseCaseView', x, y, 320, 56, uname, extra))

for aid, _ in ACTORS:
    for uid, _ in USE_CASES:
        if (aid == 'admin') != (uid == 'run-server'):
            continue
        eid = 'uc-assoc-%s-%s' % (aid, uid)
        uc_elements.append({
            "_type": "UMLAssociation", "_id": eid, "_parent": {"$ref": COURSE_MODEL},
            "end1": {"_type": "UMLAssociationEnd", "_id": eid + '-end1',
                     "_parent": {"$ref": eid}, "reference": {"$ref": "uc-actor-" + aid}},
            "end2": {"_type": "UMLAssociationEnd", "_id": eid + '-end2',
                     "_parent": {"$ref": eid}, "reference": {"$ref": "uc-use-" + uid}}})
        ax, ay = ACTOR_POS[aid]
        ux, uy = uc_pos[uid]
        uc_views.append(edge_view('uc-view-' + eid, UC_DIAGRAM, eid, 'UMLAssociationView',
                                  'uc-view-actor-' + aid, 'uc-view-use-' + uid,
                                  "%d:%d;%d:%d" % (ax + 200, ay + 35, ux, uy + 28)))

for lane, (src, dst) in enumerate(INCLUDES):
    eid = 'uc-include-%s-%s' % (src, dst)
    uc_elements.append({"_type": "UMLInclude", "_id": eid,
                        "_parent": {"$ref": "uc-use-" + src},
                        "source": {"$ref": "uc-use-" + src},
                        "target": {"$ref": "uc-use-" + dst}})
    sx, sy = uc_pos[src]
    dx, dy = uc_pos[dst]
    if dx > sx:
        points = "%d:%d;%d:%d" % (sx + 320, sy + 28, dx, dy + 28)
    elif dx < sx:
        points = "%d:%d;%d:%d" % (sx, sy + 28, dx + 320, dy + 28)
    else:
        x = sx + 180 + (lane % 3) * 45
        if dy > sy:
            points = "%d:%d;%d:%d" % (x, sy + 56, x, dy)
        else:
            points = "%d:%d;%d:%d" % (x, sy, x, dy + 56)
    uc_views.append(edge_view('uc-view-' + eid, UC_DIAGRAM, eid, 'UMLIncludeView',
                              'uc-view-use-' + src, 'uc-view-use-' + dst, points,
                              '', '«include»'))

# --------------------------------------------- use case mermaid equivalent
uc_lines = ["flowchart LR"]
for aid, aname in ACTORS:
    uc_lines.append('    %s((%s))' % ('actor_' + aid, aname))
uc_lines.append('    subgraph SYS[Backup Studio 备份系统]')
for uid, uname in USE_CASES:
    uc_lines.append('        %s["%s"]' % ('uc_' + uid.replace('-', '_'), uname))
uc_lines.append('    end')
for aid, _ in ACTORS:
    for uid, _ in USE_CASES:
        if (aid == 'admin') != (uid == 'run-server'):
            continue
        uc_lines.append('    %s --- %s' % ('actor_' + aid, 'uc_' + uid.replace('-', '_')))
for src, dst in INCLUDES:
    uc_lines.append('    %s -.->|include| %s' % ('uc_' + src.replace('-', '_'),
                                                  'uc_' + dst.replace('-', '_')))
MERMAID['use-cases'] = "\n".join(uc_lines) + "\n"

uc_diagram = {"_type": "UMLUseCaseDiagram", "_id": UC_DIAGRAM,
              "_parent": {"$ref": COURSE_MODEL},
              "name": "用例图 - Backup Studio", "visible": True,
              "ownedViews": uc_views}

# ------------------------------------------------------- sequence diagrams
SEQUENCES = [
    {
        "id": "seq-local-backup",
        "name": "时序图 - 本地备份",
        "title": "本地备份流程",
        "participants": [
            {"key": "user", "label": "用户", "class": None},
            {"key": "ui", "label": "界面层（CLI / GUI）", "class": "UiLayer"},
            {"key": "engine", "label": "归档引擎", "class": "BackupEngine"},
            {"key": "fs", "label": "本地文件系统", "class": "LocalFilesystem"},
        ],
        "messages": [
            ("user", "ui", "backup SOURCE -o ARCHIVE", "备份命令（--pack/--compress/--encrypt/--include）"),
            ("ui", "engine", "create(source, archivePath, options)", "调用归档引擎"),
            ("engine", "fs", "scanDirectory(includes)", "遍历目录树，按需过滤条目并收集元数据与硬链接"),
            ("engine", "fs", "packStream() / packIndex()", "写出 packed 暂存件"),
            ("engine", "fs", "compressStage()", "压缩（none / RLE / Huffman / LZ77）"),
            ("engine", "fs", "cryptStage()", "加密（随机盐 + ChaCha20 / AES-256-CTR）"),
            ("engine", "fs", "commitFileNoReplace()", "fsync 后原子提交最终归档"),
            ("engine", "ui", "BackupResult", "条目数、输入/输出字节数"),
            ("ui", "user", "backup completed", "成功或错误信息"),
        ],
    },
    {
        "id": "seq-local-restore",
        "name": "时序图 - 本地还原",
        "title": "本地还原流程（先校验后提取）",
        "participants": [
            {"key": "user", "label": "用户", "class": None},
            {"key": "ui", "label": "界面层（CLI / GUI）", "class": "UiLayer"},
            {"key": "engine", "label": "归档引擎", "class": "BackupEngine"},
            {"key": "fs", "label": "归档与还原目标", "class": "LocalFilesystem"},
        ],
        "messages": [
            ("user", "ui", "restore ARCHIVE -d DEST", "还原命令（--overwrite 可选）"),
            ("ui", "engine", "restore(archive, destination, options)", "调用归档引擎"),
            ("engine", "fs", "readArchiveHeader() + sha256File()", "校验 encoded 负载摘要"),
            ("engine", "fs", "cryptStage()", "解密到暂存件（口令错误即失败）"),
            ("engine", "fs", "decompressStage()", "解压并校验 packed 摘要与大小"),
            ("engine", "fs", "validateEntries()", "校验条目路径、重复与数据边界"),
            ("engine", "fs", "extractEntries()", "目标目录 FD + O_NOFOLLOW 建临时节点"),
            ("engine", "fs", "applyMetadataAt() + commitNodeAt()", "写回元数据并逐条提交（默认不覆盖）"),
            ("engine", "ui", "BackupResult", "条目数、输入/输出字节数"),
            ("ui", "user", "restore completed", "成功或错误信息"),
        ],
    },
    {
        "id": "seq-remote-upload",
        "name": "时序图 - 远程上传",
        "title": "远程上传流程（挑战-响应登录）",
        "participants": [
            {"key": "ui", "label": "界面层（CLI / GUI）", "class": "UiLayer"},
            {"key": "client", "label": "远程客户端", "class": "BackupClient"},
            {"key": "server", "label": "备份服务会话", "class": "BackupServer"},
            {"key": "store", "label": "账户与归档存储", "class": "ServerStorage"},
        ],
        "messages": [
            ("ui", "client", "login(username)", "输入账户口令"),
            ("client", "server", "LoginStart", "请求登录"),
            ("server", "client", "LoginChallenge(salt, nonce)", "返回盐与随机挑战"),
            ("client", "server", "LoginProof", "Proof = SHA256(verifier ∥ nonce)"),
            ("server", "client", "LoginResponse", "校验通过，建立已认证会话"),
            ("ui", "client", "upload(archivePath, displayName, incremental)", "本地归档完成后上传（支持增量与 TLS）"),
            ("client", "server", "UploadStart / UploadDeltaStart", "声明大小、摘要及可选基线 ID"),
            ("server", "client", "UploadReady(id)", "分配备份 ID（增量模式附带基线块哈希）"),
            ("client", "server", "UploadChunk / UploadCopy × N", "512 KiB 分帧传输或同偏移块复用引用"),
            ("client", "server", "UploadEnd", "传输完成"),
            ("server", "store", "commitStoredFile() + writeMeta()", "校验大小/SHA-256 后原子提交完整独立归档"),
            ("server", "client", "UploadResult(id)", "返回备份 ID"),
            ("client", "ui", "backupId", "上传成功（历史记录可查）"),
        ],
    },
]

seq_elements = []

for seq in SEQUENCES:
    interaction_id = seq['id'] + '-interaction'
    collab_id = seq['id'] + '-collab'
    classes = {}
    for p in seq['participants']:
        if p['class']:
            classes[p['class']] = seq['id'] + '-class-' + p['class']
    for cname, cid in classes.items():
        seq_elements.append({"_type": "UMLClass", "_id": cid,
                             "_parent": {"$ref": COURSE_MODEL}, "name": cname})
    collab = {"_type": "UMLCollaboration", "_id": collab_id,
              "_parent": {"$ref": COURSE_MODEL},
              "name": seq['title'] + "参与者", "ownedElements": []}
    lifelines = []
    for p in seq['participants']:
        attr_id = seq['id'] + '-attr-' + p['key']
        attr = {"_type": "UMLAttribute", "_id": attr_id, "_parent": {"$ref": collab_id},
                "name": p['label']}
        if p['class']:
            attr['type'] = {"$ref": classes[p['class']]}
        collab['ownedElements'].append(attr)
        lifelines.append({"_type": "UMLLifeline", "_id": seq['id'] + '-lifeline-' + p['key'],
                          "_parent": {"$ref": interaction_id},
                          "represent": {"$ref": attr_id}, "isMultiInstance": False})
    seq_elements.append(collab)

    messages = []
    for i, (src, dst, sig, detail) in enumerate(seq['messages'], start=1):
        receiver = next(p for p in seq['participants'] if p['key'] == dst)
        op_id = "%s-op-%d" % (seq['id'], i)
        if receiver['class']:
            seq_elements.append({"_type": "UMLOperation", "_id": op_id,
                                 "_parent": {"$ref": classes[receiver['class']]},
                                 "name": sig})
            signature = {"$ref": op_id}
        else:
            signature = None
        messages.append({"_type": "UMLMessage", "_id": "%s-msg-%d" % (seq['id'], i),
                         "_parent": {"$ref": interaction_id},
                         "source": {"$ref": seq['id'] + '-lifeline-' + src},
                         "target": {"$ref": seq['id'] + '-lifeline-' + dst},
                         "signature": signature})

    diagram_id = seq['id'] + '-diagram'
    diagram = {"_type": "UMLSequenceDiagram", "_id": diagram_id,
               "_parent": {"$ref": interaction_id}, "name": seq['name'],
               "visible": True, "ownedViews": []}
    interaction = {"_type": "UMLInteraction", "_id": interaction_id,
                   "_parent": {"$ref": COURSE_MODEL}, "name": seq['title'],
                   "ownedElements": [diagram], "participants": lifelines,
                   "messages": messages}
    seq_elements.append(interaction)

    n = len(seq['participants'])
    col_w, head_w, row_h = 300, 240, 46
    frame_left, frame_top = 20, 20
    frame_w = 60 + col_w * n
    head_top = frame_top + 60
    line_top = head_top + 70
    line_bottom = line_top + row_h * (len(messages) + 1)
    frame_h = (line_bottom - frame_top) + 60
    centers = [frame_left + 60 + col_w * i + head_w // 2 for i in range(n)]

    mermaid_lines = ["sequenceDiagram", "    autonumber"]
    for i, p in enumerate(seq['participants']):
        mermaid_lines.append('    participant P%d as %s' % (i, p['label']))

    diagram['ownedViews'].append({
        "_type": "UMLFrameView", "_id": seq['id'] + '-frame',
        "_parent": {"$ref": diagram_id}, "model": {"$ref": diagram_id},
        "subViews": [
            label(seq['id'] + '-frame-name', seq['id'] + '-frame', seq['name'],
                  frame_left + 25, frame_top + 5, True, 320, 14, 0),
            label(seq['id'] + '-frame-type', seq['id'] + '-frame', 'sd',
                  frame_left + 5, frame_top + 5, True, 20, 14, 0)],
        "font": "Arial;13;0", "left": frame_left, "top": frame_top,
        "width": frame_w, "height": frame_h,
        "nameLabel": {"$ref": seq['id'] + '-frame-name'},
        "frameTypeLabel": {"$ref": seq['id'] + '-frame-type'}})

    for i, p in enumerate(seq['participants']):
        lid = seq['id'] + '-lifeline-' + p['key']
        view_id = lid + '-view'
        nc = name_compartment(view_id + '-namecomp', view_id, lid, p['label'],
                              centers[i] - head_w // 2, head_top, head_w, 26)
        lp = {"_type": "UMLLinePartView", "_id": view_id + '-line',
              "_parent": {"$ref": view_id}, "model": {"$ref": lid},
              "font": "Arial;13;0", "left": centers[i], "top": line_top,
              "width": 1, "height": line_bottom - line_top}
        diagram['ownedViews'].append({
            "_type": "UMLSeqLifelineView", "_id": view_id,
            "_parent": {"$ref": diagram_id}, "model": {"$ref": lid},
            "subViews": [nc, lp], "font": "Arial;13;0",
            "left": centers[i] - head_w // 2, "top": head_top, "width": head_w,
            "height": line_bottom - head_top, "nameCompartment": {"$ref": nc['_id']},
            "linePart": {"$ref": lp['_id']}})

    index_of = {p['key']: i for i, p in enumerate(seq['participants'])}
    for j, ((src, dst, sig, detail), message) in enumerate(zip(seq['messages'], messages)):
        sid, did = index_of[src], index_of[dst]
        y = line_top + row_h * (j + 1)
        mid = "%s-msgview-%d" % (seq['id'], j + 1)
        if sid == did:
            points = "%d:%d;%d:%d" % (centers[sid], y, centers[sid] + 30, y + 20)
        else:
            points = "%d:%d;%d:%d" % (centers[sid], y, centers[did], y)
        labels, refs = edge_refs(mid, message['_id'], sig)
        labels[0]['left'] = min(centers[sid], centers[did]) + abs(
            centers[did] - centers[sid]) // 2 - 85
        labels[0]['top'] = y - 20
        labels[0]['width'] = 190
        view = {"_type": "UMLSeqMessageView", "_id": mid,
                "_parent": {"$ref": diagram_id}, "model": {"$ref": message['_id']},
                "subViews": labels, "font": "Arial;13;0",
                "head": {"$ref": seq['id'] + '-lifeline-' + dst + '-view-line'},
                "tail": {"$ref": seq['id'] + '-lifeline-' + src + '-view-line'},
                "lineStyle": 1, "points": points}
        view.update(refs)
        diagram['ownedViews'].append(view)
        text = sig if not detail else sig + "：" + detail
        mermaid_lines.append('    P%d->>P%d: %s' % (sid, did, text))

    MERMAID[seq['id']] = "\n".join(mermaid_lines) + "\n"

# ---------------------------------------------------------- deployment model
DEP_DIAGRAM = 'backup-studio-deployment'
DEP_NODES = [("client", "客户端主机（Windows / Linux）", 60, 60, 380, 280),
             ("server", "服务器主机（Linux）", 620, 60, 440, 320)]
DEP_COMPONENTS = [("cli", "client", "backup-cli（命令行客户端）", 90, 170, 320, 52),
                  ("gui", "client", "backup-gui（Qt 6 桌面端）", 90, 240, 320, 52),
                  ("service", "server", "backup-server（备份服务）", 650, 170, 380, 52),
                  ("storage", "server", "server_data/（账户与归档存储）", 650, 240, 380, 52)]

dep_elements, dep_views = [], []
for nid, nname, *_ in DEP_NODES:
    dep_elements.append({"_type": "UMLNode", "_id": "dep-node-" + nid,
                         "_parent": {"$ref": COURSE_MODEL}, "name": nname,
                         "stereotype": "device"})
for cid, owner, cname, *_ in DEP_COMPONENTS:
    dep_elements.append({"_type": "UMLComponent", "_id": "dep-comp-" + cid,
                         "_parent": {"$ref": COURSE_MODEL}, "name": cname})
for nid, nname, x, y, w, h in DEP_NODES:
    contained = ['dep-view-comp-' + cid for cid, owner, *_ in DEP_COMPONENTS if owner == nid]
    dep_views.append(node_view('dep-view-node-' + nid, DEP_DIAGRAM, 'dep-node-' + nid,
                               'UMLNodeView', x, y, w, h, nname, contained=contained))
for cid, owner, cname, x, y, w, h in DEP_COMPONENTS:
    v = node_view('dep-view-comp-' + cid, DEP_DIAGRAM, 'dep-comp-' + cid,
                  'UMLComponentView', x, y, w, h, cname)
    v['containerView'] = {"$ref": 'dep-view-node-' + owner}
    v['stereotypeDisplay'] = 'decoration-label'
    dep_views.append(v)

DEP_EDGE = 'dep-link-client-server'
dep_elements.append({"_type": "UMLCommunicationPath", "_id": DEP_EDGE,
                     "_parent": {"$ref": COURSE_MODEL}, "name": "NBKP/TCP 8848",
                     "end1": {"_type": "UMLAssociationEnd", "_id": DEP_EDGE + '-end1',
                              "_parent": {"$ref": DEP_EDGE},
                              "reference": {"$ref": "dep-node-client"}},
                     "end2": {"_type": "UMLAssociationEnd", "_id": DEP_EDGE + '-end2',
                              "_parent": {"$ref": DEP_EDGE},
                              "reference": {"$ref": "dep-node-server"}}})
dep_views.append(edge_view('dep-view-link', DEP_DIAGRAM, DEP_EDGE,
                           'UMLCommunicationPathView', 'dep-view-node-client',
                           'dep-view-node-server', '440:190;620:190', 'NBKP/TCP 8848',
                           '«TCP»'))
# ------------------------------------------- deployment mermaid equivalent
dep_lines = ["flowchart LR"]
for nid, nname, *_ in DEP_NODES:
    dep_lines.append('    subgraph %s[%s]' % ('dep_' + nid, nname))
    for cid, owner, cname, *_ in DEP_COMPONENTS:
        if owner == nid:
            dep_lines.append('        %s["%s"]' % ('dep_' + cid, cname))
    dep_lines.append('    end')
dep_lines.append('    %s == "%s（«TCP»）" ==> %s' %
                 ('dep_' + DEP_NODES[0][0], 'NBKP/TCP 8848', 'dep_' + DEP_NODES[1][0]))
MERMAID['deployment'] = "\n".join(dep_lines) + "\n"

dep_diagram = {"_type": "UMLDeploymentDiagram", "_id": DEP_DIAGRAM,
               "_parent": {"$ref": COURSE_MODEL},
               "name": "部署图 - 客户端与备份服务器", "visible": True,
               "ownedViews": dep_views}

# --------------------------------------------------------------- assemble
def ensure_class_member(cls_model, cls_view, kind, member_id, name, text,
                        attr_type=None, is_static=False, params=()):
    key = 'attributes' if kind == 'attr' else 'operations'
    comp_idx = 1 if kind == 'attr' else 2
    comp = cls_view['subViews'][comp_idx]
    members = cls_model.setdefault(key, [])
    if not any(m['_id'] == member_id for m in members):
        if kind == 'attr':
            members.append({
                "_type": "UMLAttribute", "_id": member_id,
                "_parent": {"$ref": cls_model['_id']}, "name": name,
                "visibility": "public", "type": attr_type,
            })
        else:
            op = {
                "_type": "UMLOperation", "_id": member_id,
                "_parent": {"$ref": cls_model['_id']}, "name": name,
                "visibility": "public",
            }
            if is_static:
                op["isStatic"] = True
            op["parameters"] = [
                {"_type": "UMLParameter", "_id": f"{member_id}-{pname}",
                 "_parent": {"$ref": member_id},
                 **({"name": pname} if pdir != "return" else {}),
                 "type": ptype, "direction": pdir}
                for pname, ptype, pdir in params
            ]
            members.append(op)
    v_id = member_id + '-view'
    sub = comp.setdefault('subViews', [])
    if not any(v['_id'] == v_id for v in sub):
        vtype = 'UMLAttributeView' if kind == 'attr' else 'UMLOperationView'
        top = comp.get('top', cls_view['top'] + 33) + 5 + 18 * len(sub)
        sub.append({
            "_type": vtype, "_id": v_id,
            "_parent": {"$ref": comp['_id']},
            "model": {"$ref": member_id}, "font": "Arial;13;0",
            "left": cls_view['left'] + 5, "top": top,
            "width": cls_view['width'] - 10, "height": 18, "text": text,
        })
        cls_view['height'] = max(cls_view['height'], cls_view['height'] + 18)


pub_model = next((m for m in project.get('ownedElements', [])
                  if m.get('_id') == 'backup-studio-model'), None)
if pub_model:
    cdiag = pub_model['ownedElements'][0]
    cmodels = {el['_id']: el for el in pub_model['ownedElements']
               if el.get('_type') == 'UMLClass'}
    cviews = {v['model']['$ref']: v for v in cdiag['ownedViews']
              if v.get('_type') == 'UMLClassView'}
    ensure_class_member(
        cmodels['class-BackupOptions'], cviews['class-BackupOptions'],
        'attr', 'class-BackupOptions-includes', 'includes',
        '+ includes: std::vector<std::string>', 'std::vector<std::string>')
    ensure_class_member(
        cmodels['class-BackupEngine'], cviews['class-BackupEngine'],
        'op', 'class-BackupEngine-automatic', 'automatic', '+ automatic',
        is_static=True,
        params=[('sourceDirectory', 'const std::string&', 'in'),
                ('outputDirectory', 'const std::string&', 'in'),
                ('options', 'const BackupOptions&', 'in'),
                ('automatic', 'const AutomaticOptions&', 'in'),
                ('return', 'BackupResult', 'return')])
    ensure_class_member(
        cmodels['class-BackupEngine'], cviews['class-BackupEngine'],
        'op', 'class-BackupEngine-scanSource', 'scanSource', '+ scanSource',
        is_static=True,
        params=[('sourceDirectory', 'const std::string&', 'in'),
                ('return', 'std::vector<SourceEntryInfo>', 'return')])
    ensure_class_member(
        cmodels['class-BackupClient'], cviews['class-BackupClient'],
        'op', 'class-BackupClient-login', 'login', '+ login',
        params=[('error', 'std::string&', 'inout'),
                ('cancel', 'std::atomic_bool*', 'in'),
                ('return', 'bool', 'return')])
    ensure_class_member(
        cmodels['class-BackupClient'], cviews['class-BackupClient'],
        'op', 'class-BackupClient-deleteAccount', 'deleteAccount', '+ deleteAccount',
        params=[('error', 'std::string&', 'inout'),
                ('cancel', 'std::atomic_bool*', 'in'),
                ('return', 'bool', 'return')])
    ensure_class_member(
        cmodels['class-ServerConfig'], cviews['class-ServerConfig'],
        'attr', 'class-ServerConfig-bindAddress', 'bindAddress',
        '+ bindAddress: std::string', 'std::string')
    ensure_class_member(
        cmodels['class-ServerConfig'], cviews['class-ServerConfig'],
        'attr', 'class-ServerConfig-tlsCertificate', 'tlsCertificate',
        '+ tlsCertificate: std::string', 'std::string')
    ensure_class_member(
        cmodels['class-ServerConfig'], cviews['class-ServerConfig'],
        'attr', 'class-ServerConfig-tlsPrivateKey', 'tlsPrivateKey',
        '+ tlsPrivateKey: std::string', 'std::string')

course_model = {"_type": "UMLModel", "_id": COURSE_MODEL,
                "_parent": {"$ref": "backup-studio-project"}, "name": "需求与设计模型",
                "ownedElements": ([uc_diagram] + uc_elements + [dep_diagram] +
                                  dep_elements + seq_elements)}
project['ownedElements'] = [
    el for el in project.get('ownedElements', [])
    if el.get('_id') != COURSE_MODEL
] + [course_model]
project['documentation'] = (
    "Version 1.12.0 model: public interface class diagram plus course diagrams "
    "(use case, three sequence diagrams and deployment). Executable logic is "
    "defined by the C++ source; this model omits private helpers and is not a "
    "complete API contract.")

json.dump(project, open(OUT, 'w', encoding='utf-8'), indent=2, ensure_ascii=False)
open(OUT, 'a', encoding='utf-8').write('')

os.makedirs(MERMAID_DIR, exist_ok=True)
for key, text in MERMAID.items():
    open(os.path.join(MERMAID_DIR, key + '.mmd'), 'w', encoding='utf-8').write(text)
print('written', OUT, 'and', len(MERMAID), 'mermaid files')
