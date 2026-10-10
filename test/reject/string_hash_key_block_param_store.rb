# spinel: reject-share
# A yielding method's block parameter holds a String stored under :name,
# and the Hash is then mutated through :dirs, which is given that String too
# (`options[:dirs] = options[:name]`): the mutation reaches the stored
# parameter's String. Refused, not compiled with the append lost.
def each_opt
  yield "a" + ""
end
options = { dirs: [] }
each_opt { |v| options[:name] = v }
options[:dirs] = options[:name]
options[:dirs] << "x"
p options
