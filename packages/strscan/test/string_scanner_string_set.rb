# StringScanner#string= scans another String from the start, as #reset
# leaves a scanner, and answers it (Liquid's ParseContext reuses one scanner
# for every markup it parses)
require "strscan"

ss = StringScanner.new("abc def")
p ss.scan(/\w+/)
p ss.pos
p(ss.string = "xyz 12")
p ss.pos
p ss.matched?
p ss.scan(/\w+/)
p ss.string
ss.string = "9"
p ss.eos?
p ss.scan(/\d/)
p ss.eos?
