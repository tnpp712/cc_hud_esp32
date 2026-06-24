from cchud_daemon.cli import merge_claude_settings


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
