# A constant computed with Math's functions on literals runs no code of the
# program's, so Table::DEPTH, read by a method written above it, keeps no
# presence flag.
# absent: cst_DEPTH__set
class Reader
  def self.depth; Table::DEPTH; end
end
ROOT2 = Math.sqrt(2.0)
class Table
  DEPTH = 7
end
p Reader.depth, ROOT2.round(4)
