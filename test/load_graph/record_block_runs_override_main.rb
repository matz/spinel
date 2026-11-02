# A lazy chain's block is later only when no method of the chain is one
# the program defines: a program `map`, wherever it is defined, may run
# the block as it is called (a reopened Enumerator::Lazy#map, say), so
# the record takes the block to run now, as any iterator's. The chain
# with no such name stays later.
class Q
  def map = yield
end
class S; end
held = [1].lazy.map { S.z }
kept = [1].lazy.select { S.z }
p((held.first rescue :nme))
p((kept.to_a rescue :nme))
def S.z = 1
p Q.new.map { 2 }
