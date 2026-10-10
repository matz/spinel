# String#delete_prefix has a pure boxed String row. This mixed call's user
# arm keeps the Array argument and mutates its nested String after the call.
# The zero-argument test run selects Params while retaining String in the
# receiver expression, so the boxed builtin peek must not erase that flow.
# spinel: share
class Params
  def delete_prefix(prefix)
    @saved_prefix = prefix
    nil
  end

  def saved_prefix
    @saved_prefix
  end
end

params = Params.new
source = String.new("captured")
receiver = [params, String.new("unused")][ARGV.length]
receiver.delete_prefix([source])
if ARGV.empty?
  params.saved_prefix[0] << "!"
  puts source
else
  puts "string"
end
