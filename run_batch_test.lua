if (not arg[5]) then
   print("Usage: "..arg[0].." <device> <ram(MB)> <#drives> <#data_drives> <#independent_array>")
   print("ram size will be used to make sure that:")
   print("- sequential reads are not read from the cache")
   print("- sequential and random writes are not affected by a `cache burst'")
   print("random read should be unaffected by the cache in all cases")
   print("")
   print("#drives will be used to figure the number of i/o to do for sequential and random reads")
   print("#data_drives will be used to figure the number of i/o to do for seq writes")
   print("#independent_array will be used to figure the number of i/o to do for random writes")
   print("")
   print("example:")
   print("raid6 of 7 drives : ./PROGRAM <device> <ram> 7 5 1")
   print("raid60 of 35 drives, 7 drives per raid6 : ./PROGRAM <device> <ram> 35 25 1")
   print("raid10 of 14 drives: ./PROGRAM <device> <ram> 14 7 7")
   print("")
   print("You're expected to correctly tune nr_requests and queue_depth before.")
   os.exit(1)
end

local dev, ram, drives, data_drives, independent_array = unpack(arg, 1)

function run(random, write, iosize)
   local iocount = 0
   local maxinflight = 0
   local maxsubmit = 10
   if (not random) then
      if (iosize <= 16) then
	 maxinflight = 400
      else
	 maxinflight = 200
      end
      -- figure number of i/o  to do
      local bp = 120 * (not write and drives or data_drives)
      if (bp > 1000) then bp = 1000 end
      iocount = 30 * bp * 1024 / iosize
   else
      maxinflight = 32 * drives
      iocount = 30 * 200 * (not write and data_drives or independent_array)
   end
   if (not random or write) then
      -- make cache at most 10% of the total i/o
      if (iocount * iosize < ram * 1024 * 10) then
	 iocount = ram * 1024 * 10 / iosize
      end
   end
   local cmd = "./zero_md0 --dev="..dev..(random and " --random" or "")..(write and " --write" or "")
   cmd = cmd.." --iosize="..iosize.. " --iocount="..iocount.." --maxinflight="..maxinflight.." --maxsubmit="..maxsubmit
   --   cmd = "./zero_md0 --dev=/dev/sdf --iosize=4 --iocount=1000 --maxinflight=400 --maxsumbit=10"
   print(cmd)
--   if (1) then return end
   local iops, bp
   local f = io.popen(cmd, "r")
   for line in f:lines() do
      --print(line)
      local n
      n = string.match(line, "^iop/s (%d+)$")
      if (n) then iops = n end
      n = string.match(line, "^average bandwidth : ([%d.]+) MB/s")
      if (n) then bp = n end
   end
   f:close()
   result(random, write, iosize, iocount, iops, bp)
end

function result(random, write, iosize, iocount, iops, bp)
   local text = (random and "Random" or "Sequential").." "..(write and "Write" or "Read").." de "..iosize.."k : "..iops.." iop/s ("..bp.." MB/s)"
   print(text)
   local f = io.open("./result.txt", "a+")
   f:write(text.."\n")
   f:flush()
   f:close()
end

run(nil, nil, 4)
run(nil, nil, 16)
run(nil, nil, 32)
run(nil, nil, 32)
run(nil, nil, 64)
run(nil, nil, 128)
run(nil, nil, 256)
run(nil, true, 4)
run(nil, true, 16)
run(nil, true, 32)
run(nil, true, 32)
run(nil, true, 64)
run(nil, true, 128)
run(nil, true, 256)
run(true, nil, 4)
run(true, nil, 16)
run(true, nil, 32)
run(true, nil, 32)
run(true, nil, 64)
run(true, nil, 128)
run(true, nil, 256)
run(true, true, 4)
run(true, true, 16)
run(true, true, 32)
run(true, true, 32)
run(true, true, 64)
run(true, true, 128)
run(true, true, 256)
