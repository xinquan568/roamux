# SPDX-License-Identifier: Apache-2.0
"""Generate a Sparkle appcast.xml for a Roamux release (roam-34, K2/K3).

One <item> with sparkle:version, pubDate, and an <enclosure> whose url is the
signed ARTIFACT download URL (…/releases/download/<tag>/<artifact>, never the
feed url), length is the exact artifact byte count, and sparkle:edSignature is
the EdDSA signature produced by Sparkle's sign_update (required in EVERY
signing mode). The in-app verifier (roam-32) checks the signature against
SUPublicEDKey.
"""

import argparse
import pathlib
import sys
from xml.sax.saxutils import quoteattr

TEMPLATE = """<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>Roamux</title>
    <item>
      <title>{short_version}</title>
      <sparkle:version>{version}</sparkle:version>
      <sparkle:shortVersionString>{short_version}</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>12.0</sparkle:minimumSystemVersion>
      <pubDate>{pub_date}</pubDate>
      <enclosure url={url}
                 length="{length}"
                 type="application/octet-stream"
                 sparkle:edSignature={sig}/>
    </item>
  </channel>
</rss>
"""


def generate_appcast(version, enclosure_url, artifact_bytes, ed_signature,
                     pub_date, short_version):
    # roam-141: `version` is the numeric, Sparkle-orderable CFBundleVersion the
    # comparator uses; `short_version` is the human string the update dialog shows.
    if enclosure_url.rstrip("/").endswith("appcast.xml"):
        raise ValueError("enclosure url must be the artifact, not the feed")
    from xml.sax.saxutils import escape
    return TEMPLATE.format(
        version=version, short_version=escape(short_version), pub_date=pub_date,
        url=quoteattr(enclosure_url), length=len(artifact_bytes),
        sig=quoteattr(ed_signature))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True,
                        help="numeric CFBundleVersion Sparkle compares (roam-141)")
    parser.add_argument("--short-version", required=True,
                        help="human display string for the update dialog")
    parser.add_argument("--enclosure-url", required=True)
    parser.add_argument("--artifact", required=True, type=pathlib.Path)
    parser.add_argument("--ed-signature", required=True)
    parser.add_argument("--pub-date", required=True)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    args = parser.parse_args()
    xml = generate_appcast(args.version, args.enclosure_url,
                           args.artifact.read_bytes(), args.ed_signature,
                           args.pub_date, args.short_version)
    args.out.write_text(xml)
    print(f"[ok] appcast: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
