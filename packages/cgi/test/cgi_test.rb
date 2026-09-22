require "cgi"

# Form encoding: the unreserved bytes stand, a space is "+", everything
# else is an uppercase percent escape, per byte.
puts CGI.escape("abcXYZ019-._~") == "abcXYZ019-._~"
puts CGI.escape("a b") == "a+b"
puts CGI.escape("a/b?c=d&e") == "a%2Fb%3Fc%3Dd%26e"
puts CGI.escape("é") == "%C3%A9"
puts CGI.escape("") == ""

# The URI-component form differs in the space and nothing else.
puts CGI.escapeURIComponent("a b") == "a%20b"
puts CGI.escapeURIComponent("a/b") == "a%2Fb"
puts CGI.escape_uri_component("a b") == "a%20b"

# Unescaping reads either case of hex, turns "+" back into a space, and
# leaves a "%" that does not begin an escape exactly where it was.
puts CGI.unescape("a+b%20c") == "a b c"
puts CGI.unescape("%C3%A9") == "é"
puts CGI.unescape("%c3%a9") == "é"
puts CGI.unescape("100%") == "100%"
puts CGI.unescape("%zz") == "%zz"
puts CGI.unescape("a+b%20c%2") == "a b c%2"
puts CGI.unescape("%%41") == "%A"
puts CGI.unescape("") == ""

# unescapeURIComponent leaves "+" alone.
puts CGI.unescapeURIComponent("a+b%20c") == "a+b c"
puts CGI.unescape_uri_component("a+b") == "a+b"

# HTML escaping is the five CRuby writes, with "'" in its numeric form.
puts CGI.escapeHTML("a&b\"c<d>e'f") == "a&amp;b&quot;c&lt;d&gt;e&#39;f"
puts CGI.escapeHTML("é") == "é"
puts CGI.escape_html("<b>") == "&lt;b&gt;"
puts CGI.h("<b>") == "&lt;b&gt;"
puts CGI.escapeHTML("") == ""

# Unescaping takes the five names, both numeric forms, and passes
# through anything that is not an entity -- including a bare "&" and a
# number that is not a codepoint.
puts CGI.unescapeHTML("&amp;&lt;&gt;&quot;&#39;&#x27;") == "&<>\"''"
puts CGI.unescapeHTML("&apos;") == "'"
puts CGI.unescapeHTML("a & b &amp;amp; c") == "a & b &amp; c"
puts CGI.unescapeHTML("&#233;&#xe9;") == "éé"
puts CGI.unescapeHTML("&#x1F600;") == "\u{1F600}"
puts CGI.unescapeHTML("&#;&#x;&#999999999;&amp") == "&#;&#x;&#999999999;&amp"
puts CGI.unescapeHTML("&#0;") == "\u0000"
puts CGI.unescape_html("&lt;b&gt;") == "<b>"
puts CGI.unescapeHTML("") == ""

# The two round-trip against each other on the bytes a query string
# carries.
sample = "name=A B&url=https://example.com/p?q=1+2#fragé"
puts CGI.unescape(CGI.escape(sample)) == sample
puts CGI.unescapeURIComponent(CGI.escapeURIComponent(sample)) == sample
puts CGI.unescapeHTML(CGI.escapeHTML(sample)) == sample
