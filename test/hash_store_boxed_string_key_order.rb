# frozen_string_literal: true

def choose_key(text, string_key)
  string_key ? text : :other
end

def expand_key(text)
  text << "x" * 2048
  GC.start
  2
end

def evaluate_key(key)
  GC.start
  key
end

def store(table, key, text)
  table[key] = expand_key(text)
  p table[text], table.keys.last.bytesize
end

def store_value(table, key, text)
  table[evaluate_key(key)] = expand_key(text)
end

def freeze_table(table)
  $events << :value
  GC.start
  table.freeze
  3
end

def store_in_frozen_table(table, key)
  catch(:done) do
    result = (table[key] = freeze_table(table))
    p result
  end
rescue StandardError => error
  p error.class, $events, table
end

flag = [true, false][0]
text = +"key"
table = {"seed" => 1}
store(table, choose_key(text, flag), text)

text = +"key"
table = {"seed" => 1}
p store_value(table, choose_key(text, flag), text)
p table[text], table.keys.last.bytesize

$events = []
store_in_frozen_table({"seed" => 1}, choose_key(+"key", [true, false][1]))
