# spinel: gc-minor
# A temporary row returned by to_ary survives the allocation that copies it.
row = [1, 2]

class ConvertRow
  def to_ary = [3, 4]
end

p [row, ConvertRow.new].transpose
