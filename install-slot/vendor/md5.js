// install-slot/vendor/md5.js — RFC 1321 MD5,紧凑实现(Uint8Array 进出)。
//
// 为什么需要它:esptool stub 的 handle_flash_read(flasher_stub/stub_commands.c)
// 在数据帧发完后【无条件】追加一帧 16 字节 MD5 digest(MD5Final + SLIP_send)。
// 官方 esptool.py 每次都读取并校验该帧;esptool-js 从不读取 —— 残帧滞留在传输
// 缓冲里,下一条命令把它当响应解析 → 错位 / "invalid response" → 大量读取时
// (备份)反复失败。readFlash 修复后用本模块校验 digest,同时白得每块完整性校验。
//
// 纯函数、无依赖;浏览器(<script> 引入)与 Node(require/import)双端可用。
(function (g) {
  "use strict";
  var S = [
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
  ];
  var K = new Int32Array(64);
  for (var i = 0; i < 64; i++) K[i] = (Math.abs(Math.sin(i + 1)) * 4294967296) | 0;

  function md5(bytes) {
    var len = bytes.length;
    var total = (((len + 8) >> 6) + 1) << 6; // 补位后总长:64 的倍数,尾部留 8 字节长度
    var msg = new Uint8Array(total);
    msg.set(bytes);
    msg[len] = 0x80;
    var dv = new DataView(msg.buffer);
    var bitLen = len * 8;
    dv.setUint32(total - 8, bitLen >>> 0, true);          // 低 32 位
    dv.setUint32(total - 4, Math.floor(bitLen / 4294967296), true);
    var a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    for (var off = 0; off < total; off += 64) {
      var M = new Int32Array(16);
      for (var j = 0; j < 16; j++) M[j] = dv.getInt32(off + j * 4, true);
      var A = a0, B = b0, C = c0, D = d0;
      for (var k = 0; k < 64; k++) {
        var F, gg;
        if (k < 16)       { F = (B & C) | (~B & D);  gg = k; }
        else if (k < 32)  { F = (D & B) | (~D & C);  gg = (5 * k + 1) % 16; }
        else if (k < 48)  { F = B ^ C ^ D;           gg = (3 * k + 5) % 16; }
        else              { F = C ^ (B | ~D);        gg = (7 * k) % 16; }
        F = (F + A + K[k] + M[gg]) | 0;
        A = D; D = C; C = B;
        B = (B + ((F << S[k]) | (F >>> (32 - S[k])))) | 0;
      }
      a0 = (a0 + A) | 0; b0 = (b0 + B) | 0; c0 = (c0 + C) | 0; d0 = (d0 + D) | 0;
    }
    var out = new Uint8Array(16);
    var odv = new DataView(out.buffer);
    odv.setInt32(0, a0, true); odv.setInt32(4, b0, true);
    odv.setInt32(8, c0, true); odv.setInt32(12, d0, true);
    return out;
  }

  g.__READFLASH_MD5__ = md5;
  if (typeof module !== "undefined" && module.exports) module.exports = md5;
})(typeof globalThis !== "undefined" ? globalThis : self);
