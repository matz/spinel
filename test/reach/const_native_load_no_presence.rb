# Loading IO::Buffer's package (its native declarations, and
# PAGE_SIZE = IOBufferPackage.__page_size) runs no code of the program's,
# so Table::WIDTH, read by a method written above it, keeps no presence
# flag.
# absent: cst_WIDTH__set
class Reader
  def self.width; Table::WIDTH; end
end
class Table
  WIDTH = 12
end
buf = IO::Buffer.new(4)
buf.set_value(:U8, 0, Reader.width)
p buf.get_value(:U8, 0)
