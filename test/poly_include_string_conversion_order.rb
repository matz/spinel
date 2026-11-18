# spinel: gc-stress
class Other
  def include?(arg) = false
end

class Needle
  def initialize(subject)
    @subject = subject
  end

  def to_str
    $conversions += 1
    @subject << ("z" * 1200)
    GC.start
    +"zzzz"
  end
end

def member(subject, needle)
  subject.include?(needle)
end

$conversions = 0
text = +"a"
subjects = [text, Other.new]
needles = ["a", Needle.new(text)]
p [member(subjects[0], needles[1]), text.bytesize, $conversions]
p [member(subjects[1], needles[1]), $conversions]
