# spinel: gc-minor
# Fresh results of both rescue-modifier arms keep their String handle
# through later allocations and mutation of an alias.
def fresh_fragment(fail)
  raise "missing" if fail
  "a\0b".byteslice(0, 3)
end

[false, true].each do |fail|
  result = (fresh_fragment(fail) rescue "r\0s".dup)
  other = result
  4.times { "temporary".dup }
  other << "!"
  p result.bytes
  p result.equal?(other)
  p result.frozen?
end

def retain_fragment(fragment, fail)
  raise "missing" if fail
  fragment
end

[false, true].each do |fail|
  body = +"kept\0body"
  fallback = +"kept\0fallback"
  result = (retain_fragment(body, fail) rescue fallback)
  retained = fail ? fallback : body
  p result.equal?(retained)
  retained << "!"
  p result.bytes
end
