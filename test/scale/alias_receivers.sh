#!/bin/sh
# N receiver uses of a constant that aliases a class (`A = Foo; A.bar(i)`),
# among N classes of other code. Each use asks the load record for the class
# its node names (const_node_class_index); an answer must not rebuild the
# record's point map over the whole program, as the pass that once renamed
# each use did after the previous one's edit: N uses and N rebuilds.
n=${1:-100}
cat <<'RUBY'
class Foo
  def self.bar(x) = x + 1
end
A = Foo
t = 0
RUBY
i=0
while [ "$i" -lt "$n" ]; do
  echo "class K$i"
  echo "  def m(x) = x + $i"
  echo "end"
  echo "t += A.bar($i) + K$i.new.m($i)"
  i=$((i + 1))
done
echo 'puts t'
