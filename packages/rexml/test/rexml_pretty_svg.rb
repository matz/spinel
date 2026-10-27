require "rexml/document"

# What svg-graph writes: an XMLDecl, a DOCTYPE and a stylesheet instruction
# before the root, comments, CDATA and texts, written with Document#write's
# indent (the Pretty formatter) and with a compact Pretty of width 80. The
# buffers are +"": a String literal is frozen in Spinel.

include REXML

doc = Document.new
doc << XMLDecl.new
doc << DocType.new(%q{svg PUBLIC "-//W3C//DTD SVG 1.0//EN" } +
  %q{"http://www.w3.org/TR/2001/REC-SVG-20010904/DTD/svg10.dtd"})
doc << Instruction.new("xml-stylesheet", String.new(%Q{href="graph.css" type="text/css"}))
root = doc.add_element("svg", {
  "width" => "500",
  "height" => "300",
  "viewBox" => "0 0 500 300",
  "xmlns" => "http://www.w3.org/2000/svg",
  "xmlns:xlink" => "http://www.w3.org/1999/xlink",
  "xmlns:a3" => "http://ns.adobe.com/AdobeSVGViewerExtensions/3.0/",
  "a3:scriptImplementation" => "Adobe"
})
root << Comment.new(" " + "\\" * 66)
root << Comment.new(" Created with SVG::Graph ")
root << Comment.new(" " + "/" * 66)
defs = root.add_element("defs")
root << Comment.new(" include default stylesheet if none specified ")
style = defs.add_element("style", { "type" => "text/css" })
style << CData.new("\n.svgBackground{\n  fill:#ffffff;\n}\n.graphBackground{\n  fill:#f0f0f0;\n}\n")
root << Comment.new("SVG Background")
root.add_element("rect", { "width" => "500", "height" => "300", "x" => "0", "y" => "0", "class" => "svgBackground" })
graph = root.add_element("g", { "transform" => "translate( 50 20 )" })
graph.add_element("rect", { "x" => "0", "y" => "0", "width" => "400", "height" => "250", "class" => "graphBackground" })
fg = Element.new("g")
t = fg.add_element("text", { "x" => "10", "y" => "20", "class" => "dataPointLabel" })
t.text = "12 & <13>"
t = fg.add_element("text", { "x" => "30", "y" => "40", "class" => "dataPointLabel" })
t.text = "a   long  title\twith whitespace that runs on and on past the eighty column width of the formatter"
g = Element.new("g")
g.attributes["id"] = "popup"
g.attributes["visibility"] = "hidden"
mouseover = Element.new("circle")
mouseover.add_attributes({ "cx" => "5", "cy" => "6", "r" => "10" })
g.add_element(mouseover)
fg.add_element(g)
graph.add_element(fg)
root.add_element("text", { "x" => "250", "class" => "mainTitle" }).text = "Title"

p doc.doctype.class, doc.doctype.name, doc.doctype.external_id, doc.doctype.node_type
p doc.doctype.to_s
p doc.children.map(&:node_type)

data = +""
doc.write(data, 0)
puts data
puts "--"
p doc.to_s

f = REXML::Formatters::Pretty.new(0)
f.compact = true
out = +""
f.write(root, out)
puts out
puts "--"

# Indent 2, Element#write, the Default formatter and ie_hack.
out = +""
root.elements["g"].write(out, 2)
puts out
puts "--"
out = +""
Formatters::Default.new.write(doc, out)
p out == doc.to_s
out = +""
Formatters::Default.new(true).write(mouseover, out)
p out
out = +""
Formatters::Pretty.new(2, true).write(g, out)
puts out
puts "--"

# Text wrapping: a narrow width, a deep level, compact texts that fit and
# that do not, and a word longer than the width.
words = "the quick brown fox jumps over the lazy dog " * 4
e = Element.new("p")
e.add_element("q").text = words
e.add_element("r").text = "short"
e.add_element("s").text = "unbreakable" * 10
f = Formatters::Pretty.new(4)
f.width = 30
out = +""
f.write(e, out)
puts out
puts "--"
f.compact = true
out = +""
f.write(e, out)
puts out
puts "--"
mixed = Element.new("m")
mixed.add_text("before ")
mixed.add_element("b").text = "bold"
mixed.add_text(" after")
out = +""
Formatters::Pretty.new(1).write(mixed, out)
puts out
out = +""
f = Formatters::Pretty.new(1)
f.compact = true
f.write(mixed, out)
puts out
puts "--"

# A DocType with an external ID, a cloned DocType, and DocTypes added to a
# document that already has a root.
p DocType.new("html").to_s
p DocType.new("foo", "SYSTEM").to_s
p DocType.new(["svg", "PUBLIC", "-//W3C//DTD SVG 1.0//EN", "svg10.dtd"]).to_s
p DocType.new(["a", "PUBLIC", "it's", "say \"x\""]).to_s
p DocType.new(["a", "SYSTEM", nil, "it's"]).to_s
d2 = DocType.new(["svg", "PUBLIC", "-//W3C//DTD SVG 1.0//EN", "svg10.dtd"])
p d2.clone.to_s, d2.public, d2.system
out = +""
d2.write(out, 2)
p out
h = Document.new("<html><body/></html>")
h << DocType.new("html")
p h.children.map(&:node_type), h.to_s
h2 = Document.new("<?xml version='1.0'?><html/>")
h2 << DocType.new("html")
h2 << DocType.new("xhtml")
p h2.children.map(&:node_type), h2.to_s, h2.doctype.name
out = +""
h2.write(out, 0)
puts out
p Document.new.doctype

# Pretty-printing squeezes the text it writes, in the node itself.
d3 = Document.new("<a><b>x   \n  y</b></a>")
d3.root.add_element("c").text = "p   q"
out = +""
d3.write(out, 1)
puts out
p d3.to_s
