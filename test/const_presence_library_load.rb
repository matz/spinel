# Loading a native binding's package (IO::Buffer, implicitly required here)
# and a constant computed with Math run no code of the program's: a
# constant a method written earlier reads is present at every read.
class Reader
  def self.width; Table::WIDTH; end
end
ROOT2 = Math.sqrt(2.0)
HALF = Math.log(4.0) / 2
class Table
  WIDTH = 12
end
buf = IO::Buffer.new(4)
buf.set_value(:U8, 0, Reader.width)
p buf.get_value(:U8, 0)
p ROOT2.round(6), HALF.round(6)
p IO::Buffer::PAGE_SIZE > 0
