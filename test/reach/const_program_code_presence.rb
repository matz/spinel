# A call of the program's own method before Table::SPAN's assignment may
# read it there, so the constant keeps its presence flag.
# present: cst_SPAN__set
class Reader
  def self.span; Table::SPAN; end
end
class Probe
  def self.look = (Reader.span rescue :unset)
end
FIRST = Probe.look
class Table
  SPAN = 5
end
p FIRST, Reader.span
