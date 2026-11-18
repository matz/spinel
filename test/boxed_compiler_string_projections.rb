# A String projected from Time or from a mixed Array keeps its Ruby identity
# through a later growth. Independent calls produce independent Strings.
time = Time.utc(2024)
parts = time.to_a
zone = parts[9]
zone << "x" * 90
p [parts[9].bytesize, time.to_a[9], time.deconstruct_keys([:zone])[:zone]]

fields = time.deconstruct_keys([:zone])
field = fields[:zone]
field << "y" * 90
p [fields[:zone].bytesize, time.zone]

offset = Time.new(2024, 1, 1, 0, 0, 0, "+02:00")
p [offset.to_a[9], offset.deconstruct_keys([:zone])[:zone]]

element = +"a\0b"
strings = [element]
left = strings + [1]
right = [2] + strings
left[0] << "z" * 90
p [element.bytesize, right[1].bytesize, strings[0].bytesize, right[1].getbyte(1)]

input_key = +"key"
table = {input_key => 3}
stored = table.keys[0]
pair = table.shift
p [stored.frozen?, stored.equal?(pair[0]), stored.equal?(input_key)]
p [input_key.frozen?, pair[1]]
