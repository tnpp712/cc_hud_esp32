from cchud_daemon.cli import merge_claude_settings, render_plist


def test_install_adds_hooks_and_statusline():
    s = merge_claude_settings({}, "/x/cchud-emit.sh")
    assert "Stop" in s["hooks"]
    cmd = s["hooks"]["Stop"][0]["hooks"][0]["command"]
    assert cmd.endswith("cchud-emit.sh claude Stop")
    # statusLine 指向渲染包装脚本(同目录),而非裸 emit
    assert s["statusLine"]["command"].endswith("cchud-statusline.sh")


def test_install_is_idempotent():
    s1 = merge_claude_settings({}, "/x/cchud-emit.sh")
    s2 = merge_claude_settings(s1, "/x/cchud-emit.sh")
    assert s1 == s2                              # 重复执行不增项


def test_install_preserves_third_party_hooks():
    """第三方 hook 在 merge 后应保留;cchud 项也被追加;幂等不重复。"""
    third_party_entry = {"hooks": [{"type": "command", "command": "/usr/local/bin/my-hook"}]}
    initial = {"hooks": {"Stop": [third_party_entry]}}

    s1 = merge_claude_settings(initial, "/x/cchud-emit.sh")

    stop_hooks = s1["hooks"]["Stop"]
    # 第三方 hook 仍在
    assert third_party_entry in stop_hooks, "第三方 hook 应被保留"
    # cchud hook 也加上了
    cchud_cmds = [
        h.get("command", "")
        for e in stop_hooks
        for h in e.get("hooks", [])
        if "cchud-emit.sh" in h.get("command", "")
    ]
    assert cchud_cmds, "cchud hook 应被追加"

    # 再次 merge 保持幂等
    s2 = merge_claude_settings(s1, "/x/cchud-emit.sh")
    assert s1 == s2, "重复 merge 不应增加重复条目"


def test_render_plist_substitutes_placeholders():
    """render_plist 应将所有占位符替换为实际值,无残留 __ 占位。"""
    template = (
        "<string>__PYTHON__</string>"
        "<string>__HOME__/.cchud/daemon.log</string>"
        "<string>__DAEMON__</string>"
        "<key>CCHUD_ADDR</key><string>__ADDR__</string>"
        "<key>CCHUD_USE_V7</key><string>__USE_V7__</string>"
    )
    result = render_plist(
        template,
        python="/usr/bin/python3",
        home="/Users/test",
        daemon_dir="/Users/test/host",
        addr="AA:BB:CC:DD:EE:FF",
        use_v7="1",
    )
    assert "/usr/bin/python3" in result
    assert "/Users/test/.cchud/daemon.log" in result
    assert "/Users/test/host" in result
    assert "AA:BB:CC:DD:EE:FF" in result
    assert "<string>1</string>" in result
    # 无残留占位符
    assert "__" not in result, f"仍有未替换的占位符:\n{result}"
