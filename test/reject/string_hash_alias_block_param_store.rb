# spinel: reject-share
# :dirs holds only an Array where the Hash is written, but another name
# holds the Hash (`g = options`) and moves the stored block parameter's
# String there: the mutation through :dirs reaches that String. Refused, not
# compiled with the append lost.
def each_opt
  yield "a" + ""
end
options = { dirs: [] }
g = options
each_opt { |v| options[:name] = v }
g[:dirs] = g.delete(:name)
options[:dirs] << "x"
p options
