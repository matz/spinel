# A class nothing instantiates defines each_line (as a library's StringIO
# does when it is loaded and never used). It takes the name away from a
# boxed String on neither side: the analysis types the blockless each_line
# as the String's Enumerator, which is what the emitter answers.
class Lines
  def each_line
    yield "never"
  end
end

def gather(opt)
  sum = []
  if opt.respond_to?(:each_line)
    sum.concat([*opt.each_line].reverse)
  else
    sum << opt.to_s
  end
  sum
end

p gather("a\nb\nc\n")
p gather(42)
e = ["x\ny\n", 1].first.each_line
p e.next
