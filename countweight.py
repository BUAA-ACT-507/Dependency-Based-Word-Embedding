#To count numbers from depence files
#Created by Tony.Lin
#2017/8/1

weight = [0 for x in range (0,6000)]
file = open("deps3_ww.txt")
output = open("weightcn.txt","w+")

while 1:
    line = file.readline()
    if not line:
       	break

    linepart = line.split(" ")
    for parts in linepart:
        parts = parts.split(",")
        for txt in parts:
            if (txt.isdigit()):
                weight[int(txt)] += 1

#for i in range(len(line)):
#    if (line[i].isdigit()):
#	print(line[i]a)
#print(lines)
for i in range(len(weight)):
    print>>output,weight[i]
