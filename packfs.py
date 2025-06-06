#this script will be run by platformio.ini from its native directory
import os, sys, gzip, shutil

compress = False

if os.path.isdir("pack.tmp"):
    shutil.rmtree('pack.tmp')
try:
    filelist = []
    os.makedirs('pack.tmp/data')
    # now gzip the stuff except zones.csv since this file is not served by mongoose but directly accessed:
    for file in os.listdir("data"):
        filename = os.fsdecode(file)
        if not compress:
            shutil.copy('data/' + filename, 'pack.tmp/data/' + filename)
            filelist.append('data/' + filename)
            continue
        else:
            with open('data/' + filename, 'rb') as f_in, gzip.open('pack.tmp/data/' + filename + '.gz', 'wb') as f_out:
                f_out.writelines(f_in)
            filelist.append('data/' + filename + '.gz')
            continue
    os.chdir('pack.tmp')
    cmdstring = 'python ../pack.py ' + ' '.join(filelist)
    os.system(cmdstring + '>../src/packed_fs.c')
    os.chdir('..')
except Exception as e:
    print(f"An error occurred: {str(e)}")
    sys.exit(100)
if shutil.rmtree("pack.tmp"):
    print("Failed to clean up temporary files")
    sys.exit(9)
