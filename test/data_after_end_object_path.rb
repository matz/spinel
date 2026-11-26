# DATA named through Object, as Object::DATA and ::Object::DATA, is the
# data section
puts Object::DATA.read
::Object::DATA.rewind
p ::Object::DATA.gets
__END__
object path
