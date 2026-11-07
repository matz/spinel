#!/bin/sh
# N `rescue Alias` sites (and a `raise Alias` in each) of a constant that
# aliases a class, among N classes of other code. Each site asks the load
# record for the class its node names (const_node_match_class); an answer
# must not rebuild the record's point map over the whole program, as the
# pass that once renamed each site did for each of the N uses.
n=${1:-100}
cat <<'RUBY'
class Err < StandardError; end
E = Err
t = 0
RUBY
i=0
while [ "$i" -lt "$n" ]; do
  echo "class K$i"
  echo "  def m(x) = x + $i"
  echo "end"
  echo "begin"
  echo "  raise E, \"e$i\" if K$i.new.m($i) > 0"
  echo "rescue E"
  echo "  t += 1"
  echo "end"
  i=$((i + 1))
done
echo 'puts t'
