#!/usr/bin/env python3
"""Sample EPUB for true Korean, Hindi and other CrossPoint script fonts."""
import sys
from zipfile import ZipFile, ZIP_DEFLATED, ZIP_STORED

SAMPLES = {
    "korean": ("ko", "한국어 독서", "안녕하세요. 한국어로 책을 읽습니다. 오늘 날씨가 좋아서 도서관에 갑니다. 아름다운 우리말을 화면에 표시합니다."),
    "devanagari": ("hi", "हिन्दी पढ़ने का परीक्षण", "नमस्ते। यह हिन्दी भाषा में एक छोटी पुस्तक है। हम आज किताब पढ़ रहे हैं। भारत की भाषाएँ बहुत समृद्ध हैं।"),
    "bengali": ("bn", "বাংলা বই", "নমস্কার। আমি বাংলা ভাষায় একটি বই পড়ছি। আজ সুন্দর দিন।"),
    "gujarati": ("gu", "ગુજરાતી પુસ્તક", "નમસ્તે. આ ગુજરાતી ભાષાનું પુસ્તક છે. આજે આપણે વાંચીએ છીએ."),
    "gurmukhi": ("pa", "ਪੰਜਾਬੀ ਕਿਤਾਬ", "ਸਤ ਸ੍ਰੀ ਅਕਾਲ। ਅਸੀਂ ਪੰਜਾਬੀ ਭਾਸ਼ਾ ਵਿੱਚ ਕਿਤਾਬ ਪੜ੍ਹਦੇ ਹਾਂ।"),
    "kannada": ("kn", "ಕನ್ನಡ ಪುಸ್ತಕ", "ನಮಸ್ಕಾರ. ನಾನು ಕನ್ನಡ ಭಾಷೆಯಲ್ಲಿ ಪುಸ್ತಕ ಓದುತ್ತಿದ್ದೇನೆ."),
    "malayalam": ("ml", "മലയാളം പുസ്തകം", "നമസ്കാരം. ഇത് മലയാള ഭാഷയിലെ ഒരു ചെറിയ പുസ്തകമാണ്."),
    "oriya": ("or", "ଓଡ଼ିଆ ପୁସ୍ତକ", "ନମସ୍କାର। ଆମେ ଓଡ଼ିଆ ଭାଷାରେ ପୁସ୍ତକ ପଢ଼ୁଛୁ।"),
    "sinhala": ("si", "සිංහල පොත", "ආයුබෝවන්. මේ සිංහල භාෂාවෙන් ලියූ පොතකි."),
    "tamil": ("ta", "தமிழ் புத்தகம்", "வணக்கம். நான் தமிழ் மொழியில் ஒரு புத்தகம் படிக்கிறேன்."),
    "telugu": ("te", "తెలుగు పుస్తకం", "నమస్కారం. నేను తెలుగు భాషలో పుస్తకం చదువుతున్నాను."),
    "arabic": ("ar", "القراءة بالعربية", "مرحبًا، نحن نقرأ كتابًا باللغة العربية. هذه كلمات وجمل للاختبار."),
    "hebrew": ("he", "ספר בעברית", "שלום. אנחנו קוראים ספר בעברית. זהו מבחן של האותיות."),
}
script, target = sys.argv[1:]
lang, title, text = SAMPLES[script]
container = """<?xml version="1.0"?>
<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0">
<rootfiles><rootfile full-path="OEBPS/book.opf"
media-type="application/oebps-package+xml"/></rootfiles></container>"""
opf = f"""<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier>crosspod-{script}</dc:identifier>
<dc:title>{title}</dc:title><dc:language>{lang}</dc:language></metadata>
<manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest>
<spine><itemref idref="chapter"/></spine></package>"""
dir_attr = ' dir="rtl"' if script in ("arabic", "hebrew") else ""
chapter = f"""<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="{lang}">
<head><title>{title}</title></head><body{dir_attr}>
<h1>{title}</h1><p>{text}</p><p>{text}</p>
</body></html>"""
with ZipFile(target, "w") as z:
    z.writestr("mimetype", "application/epub+zip", compress_type=ZIP_STORED)
    z.writestr("META-INF/container.xml", container, compress_type=ZIP_DEFLATED)
    z.writestr("OEBPS/book.opf", opf, compress_type=ZIP_DEFLATED)
    z.writestr("OEBPS/chapter.xhtml", chapter, compress_type=ZIP_DEFLATED)
