// Unit tests for the portable LOPreview core (src/lop_core.h).
//
// Build & run:  ./tests/run-tests.sh   (or: g++ -std=c++20 -O2 -o t tests/test_core.cpp && ./t)
//
// Everything tested here is logic that both the prevhost proxy and the broker
// rely on, including the exact byte layout of ODF ZIP headers that Windows
// hands us through IStream.

#include <cstdio>
#include <string>
#include <vector>

#include "../src/lop_core.h"

using namespace lop;

static int g_checks = 0;
static int g_failures = 0;
static const char* g_group = "";

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("FAIL [%s] %s:%d: %s\n", g_group, __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        ++g_checks;                                                              \
        auto va = (a);                                                           \
        auto vb = (b);                                                           \
        if (!(va == vb)) {                                                       \
            ++g_failures;                                                        \
            std::printf("FAIL [%s] %s:%d: %s != %s\n", g_group, __FILE__, __LINE__, \
                        #a, #b);                                                 \
        }                                                                        \
    } while (0)

#define GROUP(name) g_group = name

// --- helpers ---------------------------------------------------------------

// Builds a ZIP local file header + payload exactly like the ODF spec mandates
// for the first entry (stored, name "mimetype").
static std::vector<unsigned char> MakeOdfZip(const std::string& mimetype) {
    std::vector<unsigned char> out;
    auto u16 = [&](uint16_t v) {
        out.push_back(static_cast<unsigned char>(v & 0xFF));
        out.push_back(static_cast<unsigned char>(v >> 8));
    };
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>(v >> (8 * i)));
    };
    u32(0x04034b50);                       // local file header signature
    u16(20);                               // version needed
    u16(0);                                // flags
    u16(0);                                // method: stored
    u16(0); u16(0);                        // time, date
    u32(0);                                // crc32 (unused by the sniffer)
    u32(static_cast<uint32_t>(mimetype.size()));  // compressed size
    u32(static_cast<uint32_t>(mimetype.size()));  // uncompressed size
    u16(8);                                // name length
    u16(0);                                // extra length
    out.insert(out.end(), {'m', 'i', 'm', 'e', 't', 'y', 'p', 'e'});
    out.insert(out.end(), mimetype.begin(), mimetype.end());
    // A little real ZIP payload after it (e.g. content.xml) to be realistic.
    out.insert(out.end(), {0x50, 0x4B, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00});
    return out;
}

static std::vector<unsigned char> MakeBytes(const std::string& s) {
    return std::vector<unsigned char>(s.begin(), s.end());
}

// --- tests -----------------------------------------------------------------

static void TestStrings() {
    GROUP("strings");
    CHECK_EQ(ExtensionOf(std::string("C:\\Users\\Dante\\My File.ODT")), std::string(".odt"));
    CHECK_EQ(ExtensionOf(std::string("../dir.d/report.ods")), std::string(".ods"));
    CHECK_EQ(ExtensionOf(std::string("noext")), std::string(""));
    CHECK_EQ(ExtensionOf(std::string("archive.tar.gz")), std::string(".gz"));
    CHECK_EQ(FileNameOf(std::string("C:\\a b\\c.odt")), std::string("c.odt"));
    CHECK_EQ(FileNameOf(std::string("c.odt")), std::string("c.odt"));
    CHECK_EQ(DirectoryOf(std::string("C:\\a b\\c.odt")), std::string("C:\\a b"));
    CHECK_EQ(DirectoryOf(std::string("c.odt")), std::string(""));

    CHECK(IsOdfExtension(".odt"));
    CHECK(IsOdfExtension(".odf"));
    CHECK(IsOdfExtension(".ott"));
    CHECK(!IsOdfExtension(".txt"));
    CHECK(!IsOdfExtension(""));

    CHECK_EQ(MimetypeToExtension("application/vnd.oasis.opendocument.text"), std::string(".odt"));
    CHECK_EQ(MimetypeToExtension("application/vnd.oasis.opendocument.spreadsheet"),
             std::string(".ods"));
    CHECK_EQ(MimetypeToExtension("application/vnd.oasis.opendocument.presentation"),
             std::string(".odp"));
    CHECK_EQ(MimetypeToExtension("application/vnd.oasis.opendocument.graphics"),
             std::string(".odg"));
    CHECK_EQ(MimetypeToExtension("application/vnd.oasis.opendocument.formula"),
             std::string(".odf"));
    CHECK_EQ(MimetypeToExtension("application/pdf"), std::string(""));

    CHECK_EQ(ToLowerAscii(std::string("ABC.Zz9")), std::string("abc.zz9"));
}

static void TestUtf8() {
    GROUP("utf8");
    const std::string samples[] = {
        "plain", "with space", "Dante", "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82",
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "\xF0\x9F\x98\x80 emoji", "a\xE2\x82\xAC" "b",
    };
    for (const std::string& s : samples) {
        CHECK_EQ(WideToUtf8(Utf8ToWide(s)), s);
    }
    // Invalid UTF-8 must not crash and must not round-trip silently.
    std::string bad = "a\xFF\xFE" "b";
    const std::wstring w = Utf8ToWide(bad);
    CHECK(!w.empty());
    // Hex round trip
    for (const std::string& s : samples) {
        std::string back;
        CHECK(HexDecode(HexEncode(s), back));
        CHECK_EQ(back, s);
    }
    std::string junk;
    CHECK(!HexDecode("ABC", junk));
    CHECK(!HexDecode("zz", junk));
    CHECK_EQ(HexEncode("AB"), std::string("4142"));
    std::string decoded;
    CHECK(HexDecode("4142", decoded));
    CHECK_EQ(decoded, std::string("AB"));
}

static void TestHashing() {
    GROUP("hash");
    // Standard FNV-1a 64 test vectors.
    CHECK_EQ(Fnv1a64(std::string("")), 0xcbf29ce484222325ULL);
    CHECK_EQ(Fnv1a64(std::string("a")), 0xaf63dc4c8601ec8cULL);
    CHECK_EQ(Fnv1a64(std::string("foobar")), 0x85944171f73967e8ULL);

    const std::string k1 = CacheKeyHex("c:\\users\\dante\\a.odt", 100, 1000);
    const std::string k2 = CacheKeyHex("c:\\users\\dante\\a.odt", 100, 1000);
    CHECK_EQ(k1.size(), static_cast<size_t>(16));
    CHECK_EQ(k1, k2);
    CHECK(k1 != CacheKeyHex("c:\\users\\dante\\a.odt", 101, 1000));   // size matters
    CHECK(k1 != CacheKeyHex("c:\\users\\dante\\a.odt", 100, 1001));   // mtime matters
    CHECK(k1 != CacheKeyHex("c:\\users\\dante\\b.odt", 100, 1000));   // path matters
    CHECK(k1 != CacheKeyHex("c:\\users\\dante\\a.odt", 100, 1000, 7));  // content matters
    CHECK(ContentKeyHex(7) != ContentKeyHex(8));
}

static void TestSniffZip() {
    GROUP("sniff-zip");
    struct Case {
        const char* mime;
        const char* ext;
    };
    const Case cases[] = {
        {"application/vnd.oasis.opendocument.text", ".odt"},
        {"application/vnd.oasis.opendocument.spreadsheet", ".ods"},
        {"application/vnd.oasis.opendocument.presentation", ".odp"},
        {"application/vnd.oasis.opendocument.graphics", ".odg"},
        {"application/vnd.oasis.opendocument.formula", ".odf"},
    };
    for (const Case& c : cases) {
        const std::vector<unsigned char> zip = MakeOdfZip(c.mime);
        const SniffResult r = SniffDocument(zip.data(), zip.size());
        CHECK(r.Recognised());
        CHECK_EQ(r.ext, std::string(c.ext));
        CHECK_EQ(r.mime, std::string(c.mime));
        CHECK(!r.flat);
    }
    // A .odt renamed to .ods still converts as a text document (content wins).
    const std::vector<unsigned char> odt = MakeOdfZip("application/vnd.oasis.opendocument.text");
    CHECK_EQ(SniffDocument(odt.data(), odt.size(), ".ods").ext, std::string(".odt"));

    // Plain ZIP (no mimetype first entry) must never be treated as ODF.
    std::vector<unsigned char> plainZip = {0x50, 0x4B, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00};
    plainZip.insert(plainZip.end(), 40, 0x41);
    SniffResult r = SniffDocument(plainZip.data(), plainZip.size());
    CHECK(!r.Recognised());
    CHECK(r.kind == SniffKind::ZipOther);

    // Truncated ODF header: not recognised (falls back to the name hint path).
    SniffResult t = SniffDocument(odt.data(), 20, ".odt");
    CHECK(!t.Recognised());

    // Also check a mimetype that is not ODF (e.g. EPUB).
    const std::vector<unsigned char> epub = MakeOdfZip("application/epub+zip");
    SniffResult e = SniffDocument(epub.data(), epub.size());
    CHECK(!e.Recognised());
    CHECK(e.kind == SniffKind::ZipOther);
    CHECK_EQ(e.mime, std::string("application/epub+zip"));
}

static void TestSniffFlatAndOther() {
    GROUP("sniff-other");
    const std::string flat =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<office:document xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
        "office:version=\"1.3\" office:mimetype=\"application/vnd.oasis.opendocument.text\">";
    SniffResult r = SniffDocument(MakeBytes(flat).data(), flat.size());
    CHECK(r.Recognised());
    CHECK(r.flat);
    CHECK_EQ(r.ext, std::string(".fodt"));

    const std::string flatOdg =
        "<?xml version=\"1.0\"?><office:document "
        "office:mimetype=\"application/vnd.oasis.opendocument.graphics\">";
    CHECK_EQ(SniffDocument(MakeBytes(flatOdg).data(), flatOdg.size()).ext, std::string(".fodg"));

    // Flat ODF without the mimetype attribute but with the right extension.
    const std::string flatNoMime = "<?xml version=\"1.0\"?><office:document><office:body/></office:document>";
    SniffResult f = SniffDocument(MakeBytes(flatNoMime).data(), flatNoMime.size(), ".fodt");
    CHECK(f.Recognised());
    CHECK_EQ(f.ext, std::string(".fodt"));
    // ... but never when the extension is not a flat ODF one.
    CHECK(!SniffDocument(MakeBytes(flatNoMime).data(), flatNoMime.size(), ".odt").Recognised());

    // SVG / XML that is not ODF: pass through.
    const std::string svg = "<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\"/>";
    CHECK(!SniffDocument(MakeBytes(svg).data(), svg.size()).Recognised());

    // Existing PDFs and unknown binaries: pass through untouched.
    const std::string pdf = "%PDF-1.7\n1 0 obj\n<<>>\nendobj\ntrailer\n";
    SniffResult p = SniffDocument(MakeBytes(pdf).data(), pdf.size());
    CHECK(p.kind == SniffKind::Pdf);
    const std::string bin = "\x7F\x45\x4C\x46\x02\x01\x01";
    CHECK(SniffDocument(MakeBytes(bin).data(), bin.size()).kind == SniffKind::Unknown);
    CHECK(SniffDocument(nullptr, 0).kind == SniffKind::TooBig);
}

static void TestScratchNames() {
    GROUP("scratch-names");
    CHECK(IsSafeScratchName("1234abcd.odt"));
    CHECK(IsSafeScratchName("0f3a9c11.fodt"));
    CHECK(!IsSafeScratchName(""));
    CHECK(!IsSafeScratchName(".."));
    CHECK(!IsSafeScratchName("...."));
    CHECK(!IsSafeScratchName("a/../b.odt"));
    CHECK(!IsSafeScratchName("..\\..\\evil.odt"));
    CHECK(!IsSafeScratchName("C:\\tmp\\evil.odt"));
    CHECK(!IsSafeScratchName("evil.odt:stream"));
    CHECK(!IsSafeScratchName("evil.exe"));
    CHECK(!IsSafeScratchName("sp ace.odt"));
    CHECK(IsSafeScratchName("dot.dot.odt"));       // dots inside the name are fine
    CHECK(!IsSafeScratchName("evil.bat"));
    CHECK(!IsSafeScratchName(std::string(200, 'a') + ".odt"));
    CHECK(ScratchBaseName(0x123456789abcdef0ULL, ".odt").size() > 4);
    CHECK(IsSafeScratchName(ScratchBaseName(0x123456789abcdef0ULL, ".odt")));
}

static void TestIni() {
    GROUP("ini");
    const std::string text =
        "\xEF\xBB\xBF; comment\n"
        "# another\n"
        "timeout_seconds = 90\n"
        "cache_max_mb=512\n"
        "direct_fallback = 1\n"
        "use_broker=yes\n"
        "log_level 3\n"          // no '=' -> ignored
        "empty=\n"
        "timeout_seconds=120\n";  // last wins
    const Ini ini = ParseIni(text);
    CHECK_EQ(ini.GetInt("timeout_seconds", 60), 120);
    CHECK_EQ(ini.GetInt("cache_max_mb", 1), 512);
    CHECK_EQ(ini.GetInt("missing", 77), 77);
    CHECK_EQ(ini.GetInt("empty", 77), 77);
    CHECK_EQ(ini.GetInt("log_level", 2), 2);
    CHECK(ini.GetBool("direct_fallback", false));
    CHECK(ini.GetBool("use_broker", false));
    CHECK(!ini.GetBool("missing", false));
    CHECK(ini.GetBool("empty", true));  // unparsable -> default
    CHECK_EQ(ini.Get("timeout_seconds", ""), std::string("120"));
    // A negative/garbage value must not wrap around.
    CHECK_EQ(ParseIni("x=-5").GetInt("x", 3), -5);
    CHECK_EQ(ParseIni("x=abc").GetInt("x", 3), 3);
    CHECK_EQ(ParseIni("x=9999999999999").GetInt("x", 3), 3);
}

static void TestUrls() {
    GROUP("urls");
    CHECK_EQ(FileUrlFromPathEscaped("C:\\Program Files\\LibreOffice"),
             std::string("file:///C:/Program%20Files/LibreOffice"));
    CHECK_EQ(FileUrlFromPathEscaped("C:\\Users\\Dante\\AppData\\Local\\LOPreview\\lo-profile\\p0"),
             std::string("file:///C:/Users/Dante/AppData/Local/LOPreview/lo-profile/p0"));
    // Unicode is percent-encoded as UTF-8 bytes, never as raw bytes.
    const std::string ru = "\xD0\x9F\xD1\x80\xD0\xB8";
    const std::string url = FileUrlFromPathEscaped("C:\\" + ru);
    CHECK_EQ(url, std::string("file:///C:/%D0%9F%D1%80%D0%B8"));
    CHECK(url.find(' ') == std::string::npos);
    CHECK_EQ(PercentEncodeUrl("a b#c"), std::string("a%20b%23c"));
}

static void TestProtocol() {
    GROUP("protocol");
    Request req;
    req.id = 0xDEADBEEF;
    req.seq = 7;
    req.clientPid = 4242;
    req.mode = "scratch";
    req.nameUtf8 = "0f3a9c11.odt";
    req.extLower = ".odt";
    req.size = 123456789;
    req.mtimeUnixMs = 1757500000123ULL;
    req.keyHex = "0123456789abcdef";
    req.convertOriginal = false;
    const std::string wire = BuildRequest(req);
    CHECK(wire.rfind("LOPREQ 1\n", 0) == 0);
    CHECK(wire.size() > 20);
    CHECK(wire[wire.size() - 1] == '\n');
    CHECK(wire.find("\n\n") != std::string::npos);

    Request parsed;
    std::string err;
    CHECK(ParseRequest(wire, parsed, err));
    CHECK_EQ(parsed.id, req.id);
    CHECK_EQ(parsed.seq, req.seq);
    CHECK_EQ(parsed.clientPid, req.clientPid);
    CHECK_EQ(parsed.mode, req.mode);
    CHECK_EQ(parsed.nameUtf8, req.nameUtf8);
    CHECK_EQ(parsed.extLower, req.extLower);
    CHECK_EQ(parsed.size, req.size);
    CHECK_EQ(parsed.mtimeUnixMs, req.mtimeUnixMs);
    CHECK_EQ(parsed.keyHex, req.keyHex);

    // Unicode path mode round trip.
    Request pathReq;
    pathReq.mode = "path";
    pathReq.nameUtf8 = "C:\\Users\\Dante\\\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\\a b.odt";
    pathReq.extLower = ".odt";
    pathReq.convertOriginal = true;
    Request parsedPath;
    CHECK(ParseRequest(BuildRequest(pathReq), parsedPath, err));
    CHECK_EQ(parsedPath.nameUtf8, pathReq.nameUtf8);
    CHECK(parsedPath.convertOriginal);

    // Rejections.
    CHECK(!ParseRequest("GET / HTTP/1.0\n\n", parsed, err));
    CHECK(!ParseRequest("LOPREQ 2\nid=00\n\n", parsed, err));
    CHECK(!ParseRequest("LOPREQ 1\nmode=path\nname=4142\n\n", parsed, err));  // no terminator
    CHECK(!ParseRequest("LOPREQ 1\nid=zz\nmode=path\nname=4142\n\n", parsed, err));
    CHECK(!ParseRequest("LOPREQ 1\nid=00\nmode=evil\nname=4142\n\n", parsed, err));
    CHECK(!ParseRequest("LOPREQ 1\nid=00\nmode=scratch\nname=2E2E2F2E2E2F78.odt\n\n", parsed, err));
    CHECK(!ParseRequest("LOPREQ 1\nid=00\nmode=scratch\nname=6162632E657865\n\n", parsed, err));
    // name=4142 is "AB": not an absolute path -> rejected
    CHECK(!ParseRequest("LOPREQ 1\nid=00\nmode=path\nname=4142\next=.odt\n\n", parsed, err));
    // UNC / device paths must never reach the broker (no network access)
    std::string unc = "LOPREQ 1\nid=00\nmode=path\nname=" +
                      HexEncode("\\\\server\\share\\a.odt") + "\next=.odt\n\n";
    CHECK(!ParseRequest(unc, parsed, err));
    std::string dev = "LOPREQ 1\nid=00\nmode=path\nname=" +
                      HexEncode("\\\\.\\C:\\a.odt") + "\next=.odt\n\n";
    CHECK(!ParseRequest(dev, parsed, err));
    // A relative path is rejected as well.
    std::string rel = "LOPREQ 1\nid=00\nmode=path\nname=" +
                      HexEncode("docs\\a.odt") + "\next=.odt\n\n";
    CHECK(!ParseRequest(rel, parsed, err));

    Response res;
    res.id = 0x11223344;
    res.status = kStatusOk;
    res.pdfFullPathUtf8 = "C:\\Users\\Dante\\AppData\\Local\\LOPreview\\cache\\0123456789abcdef.pdf";
    res.pdfSize = 4096;
    res.messageUtf8 = "converted in 1234 ms";
    Response parsedRes;
    CHECK(ParseResponse(BuildResponse(res), parsedRes, err));
    CHECK_EQ(parsedRes.id, res.id);
    CHECK_EQ(parsedRes.status, res.status);
    CHECK_EQ(parsedRes.pdfFullPathUtf8, res.pdfFullPathUtf8);
    CHECK_EQ(parsedRes.pdfSize, res.pdfSize);
    CHECK_EQ(parsedRes.messageUtf8, res.messageUtf8);

    Response failRes;
    failRes.id = 1;
    failRes.status = kStatusFail;
    failRes.messageUtf8 = "LibreOffice not found";
    Response parsedFail;
    CHECK(ParseResponse(BuildResponse(failRes), parsedFail, err));
    CHECK_EQ(parsedFail.status, kStatusFail);
    CHECK(parsedFail.pdfFullPathUtf8.empty());

    CHECK(!ParseResponse("LOPRES 1\nstatus=0\n\n", parsedRes, err));  // ok without pdf

    // The cache key ends up as a file name on the broker side: it must be
    // exactly 16 hex digits, never a path fragment.
    Request keyReq;
    keyReq.mode = "scratch";
    keyReq.nameUtf8 = "0f3a9c11.odt";
    keyReq.extLower = ".odt";
    keyReq.keyHex = "../../evil";
    CHECK(!ParseRequest(BuildRequest(keyReq), parsed, err));
    keyReq.keyHex = "0123456789abcde";  // 15 digits
    CHECK(!ParseRequest(BuildRequest(keyReq), parsed, err));
    keyReq.keyHex = "0123456789abcdeZ";
    CHECK(!ParseRequest(BuildRequest(keyReq), parsed, err));
    keyReq.keyHex = "0123456789ABCDEF";
    CHECK(ParseRequest(BuildRequest(keyReq), parsed, err));
    CHECK_EQ(parsed.keyHex, std::string("0123456789ABCDEF"));
}

static void TestPdfWriter() {
    GROUP("pdf");
    std::vector<std::string> lines = WrapText(
        "LOPreview could not preview this document.\n"
        "LibreOffice was not found (checked registry, PATH and the standard install paths).\n"
        "Install LibreOffice or set soffice_path in config.ini.\n"
        "A very long unbroken token: ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz",
        90);
    CHECK(lines.size() >= 4);
    for (const std::string& l : lines) CHECK(l.size() <= 90);

    const std::string pdf = BuildTextPdf(lines);
    std::string err;
    CHECK(ValidatePdfStructure(pdf, err));
    if (!err.empty()) std::printf("      pdf validation error: %s\n", err.c_str());
    CHECK(LooksLikePdf(reinterpret_cast<const unsigned char*>(pdf.data()), pdf.size(), &err));

    // Escaping must survive parentheses/backslashes.
    const std::string tricky = BuildTextPdf({"a (b) \\ c", "x\ty"});
    CHECK(ValidatePdfStructure(tricky, err));

    // Empty input still produces a valid page.
    const std::string empty = BuildTextPdf({});
    CHECK(ValidatePdfStructure(empty, err));

    // Ordering of wrap: words are preserved.
    const std::vector<std::string> wrapped =
        WrapText("alpha beta gamma delta epsilon zeta eta theta iota kappa", 20);
    CHECK(wrapped.size() >= 3);
    for (const std::string& l : wrapped) CHECK(l.size() <= 20);
    std::string joined;
    for (const std::string& l : wrapped) joined += l + " ";
    CHECK(joined.find("alpha beta") != std::string::npos);
    CHECK(joined.find("kappa") != std::string::npos);

    // LooksLikePdf must reject the obvious failures.
    CHECK(!LooksLikePdf(reinterpret_cast<const unsigned char*>("%PDF-1.4"), 8, &err));
    const std::string noEof(pdf.substr(0, pdf.size() - 12) + "              ");
    CHECK(!LooksLikePdf(reinterpret_cast<const unsigned char*>(noEof.data()), noEof.size(), &err));
    const std::string notPdf(200, 'x');
    CHECK(!LooksLikePdf(reinterpret_cast<const unsigned char*>(notPdf.data()), notPdf.size(), &err));
}

static void TestPdfValidatorCatchesCorruption() {
    GROUP("pdf-validator");
    const std::string good = BuildTextPdf({"hello world"});
    std::string err;
    CHECK(ValidatePdfStructure(good, err));

    // Corrupt the first in-use xref entry's offset: the validator must notice.
    const size_t nMark = good.find("00000 n \n");
    CHECK(nMark != std::string::npos);
    CHECK(nMark >= 11);
    std::string broken = good;
    broken.replace(nMark - 11, 10, "0000000000");  // offset 0 -> points at "%PDF"
    CHECK(!ValidatePdfStructure(broken, err));

    // Truncated file.
    CHECK(!ValidatePdfStructure(good.substr(0, good.size() / 2), err));
}

int main() {
    TestStrings();
    TestUtf8();
    TestHashing();
    TestSniffZip();
    TestSniffFlatAndOther();
    TestScratchNames();
    TestIni();
    TestUrls();
    TestProtocol();
    TestPdfWriter();
    TestPdfValidatorCatchesCorruption();

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
