#!/usr/bin/env python3
"""tests/test_meta_net_contract.py —— Wi-Fi 导入页 JS 与 C 路由注册的契约一致性。

历史 bug(6e51217 修复):页面 pair() 用 fetch 默认 GET 请求 /api/session,
而 ESP httpd 只注册 HTTP_POST → 405,配对码从未到达校验逻辑。
本测试把"页面 JS 调用的每个 API 路径 + 方法必须在 C 侧注册表中存在且方法一致"
固化为门禁,防止两套代码再次漂移。

解析假设(与 meta_net.c 的既有写法绑定):
- INDEX_HTML 是相邻字符串字面量拼接,JS 请求只经 fetch('PATH'...) 或 x.open('METHOD','PATH'...);
- 路由注册表为 const httpd_uri_t 名字 = { "PATH", HTTP_METHOD, handler, NULL }。
写法改变导致解析不到任何条目时测试直接失败(防静默失效)。
"""
import re
import unittest
from pathlib import Path

META_NET = Path(__file__).resolve().parent.parent / "main" / "meta_net.c"


def extract_index_html(src: str) -> str:
    # 字面量内部含 ';'(如 CSS),只能按"连续字符串行"切块,不能搜第一个分号
    m = re.search(r'INDEX_HTML\[\] =\n((?:[ \t]*"(?:[^"\\]|\\.)*";?\n)+)', src)
    if not m:
        raise AssertionError("未找到 INDEX_HTML 定义")
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
    if not parts:
        raise AssertionError("INDEX_HTML 不含字符串字面量")
    return "".join(p.encode().decode("unicode_escape") for p in parts)


def extract_routes(src: str) -> dict[str, str]:
    routes = {}
    for path, method in re.findall(
        r'httpd_uri_t\s+\w+\s*=\s*\{\s*"([^"]+)"\s*,\s*HTTP_(GET|POST|PUT|DELETE)', src
    ):
        routes[path] = method
    return routes


def extract_js_calls(html: str) -> list[tuple[str, str]]:
    calls = []
    # fetch('/path...') 或 fetch('/path...', {method:'POST'});fetch 默认 GET
    for m in re.finditer(r"fetch\('([^'?]+)[^)]*?\)", html):
        path, args = m.group(1), m.group(0)
        mm = re.search(r"method\s*:\s*'(\w+)'", args)
        calls.append((path, mm.group(1).upper() if mm else "GET"))
    # x.open('POST','/path...')
    for m in re.finditer(r"\.open\('(\w+)'\s*,\s*'([^'?]+)", html):
        calls.append((m.group(2), m.group(1).upper()))
    return calls


class TestMetaNetContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.src = META_NET.read_text()
        cls.html = extract_index_html(cls.src)
        cls.routes = extract_routes(cls.src)

    def test_routes_parsed(self):
        self.assertGreaterEqual(len(self.routes), 3, "路由注册表解析为空,解析假设已失效")

    def test_js_calls_parsed(self):
        calls = extract_js_calls(self.html)
        self.assertGreaterEqual(len(calls), 2, "JS API 调用解析为空,解析假设已失效")

    def test_every_js_call_matches_registered_route(self):
        """页面 JS 的每个 API 调用,路径必须注册且方法一致(405 回归)。"""
        for path, method in extract_js_calls(self.html):
            with self.subTest(path=path, method=method):
                self.assertIn(path, self.routes, f"JS 调用了未注册的 {path}")
                self.assertEqual(
                    self.routes[path], method,
                    f"{path}: JS 用 {method},C 注册 {self.routes[path]} → ESP httpd 会返回 405",
                )


if __name__ == "__main__":
    unittest.main(verbosity=2)
