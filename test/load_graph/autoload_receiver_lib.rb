puts "receiver lib loaded"
module AutoReceiver
end
p AutoReceiver.autoload(:Z, File.join(__dir__, "autoload_receiver_z_lib"))
p AutoReceiver::Z
