# When a block runs, as the syntax tells (--dump-load-record's block
# lines): a builtin lazy chain's block whose value leaves its expression
# unforced, and an END body, run later, at a time the syntax does not tell
# (the chain may be forced before a def that follows, the program may exit
# before it), so the record binds no such def to their calls and places
# them as before; a builtin iterator on a literal runs its block now; any
# other iterator's is taken to run now, a Proc's later.
class S; end
held = [1].lazy.map { S.z }
p((held.first rescue :nme))
p(([1].lazy.map { S.z }.first rescue :nme))
p(([1].lazy.select { S.z }).to_a) rescue p :nme
[1].each { p((S.z rescue :nme)) }
1.tap { p((S.z rescue :nme)) }
pr = proc { S.z }
END { p :end }
def S.z = 1
p pr.call
p [2].lazy.map { S.z }.first
