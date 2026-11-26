# DATA reads the text after the __END__ line; its pos counts from the start
# of the file, and rewind reads the program, as CRuby's DATA does; a BEGIN
# block reads it too
BEGIN { $early = DATA.pos > 0 }
note = <<~TEXT
  a heredoc line
__END__
  is text
TEXT
p note.lines.size
p "#{1 + 1} \"q\" \\"
start = DATA.pos
p start > 0
DATA.each_line { |line| p line.chomp }
p DATA.eof?
DATA.pos = start
p DATA.read
DATA.rewind
p DATA.pos
p DATA.gets
p DATA.lineno
p $early
DATA.seek(-4, IO::SEEK_END)
p DATA.read.bytes
__END__
first line
cafÃ© #{x} "y" \z
ÿþ
