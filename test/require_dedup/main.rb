# Both lib/types and lib/data are required directly. lib/data internally
# requires lib/types too. Without dedup, struct sp_DT_Vertex_s would be
# emitted twice and the C compile would fail with "redefinition".
require_relative 'lib/types'
require_relative 'lib/data'

puts DATA.length
DATA.each { |v| puts "#{v.x},#{v.y}" }
