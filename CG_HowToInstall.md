# Installare Yocto e dipendenze su Linux (debian based distro)
## clang 9
Preso da: https://askubuntu.com/questions/1198087/how-to-set-clang-9-as-the-default-c-compiler-on-ubuntu-19-10  

Scaricare tramite il gestore dei pacchetti clang:
```sh
sudo apt install clang-9
```
Controllare la versione
```sh
clang --version
```

Aggiornare il puntamento a clang con la versione 9:
```sh
sudo update-alternatives --install /usr/bin/c++ c++ /usr/bin/c++ 40
sudo update-alternatives --install /usr/bin/c++ c++ /usr/bin/clang++-9 60
sudo update-alternatives --config c++
```

L'ultimo comando mostra un menù a tendina per verificare che il clang selezionato di default sia la versione 9, se sì premere enter.

## cmake
Ripulire versioni già presenti di cmake, controllare con:
```sh
cmake --version
```

Se presente una versione di cmake, pulire con:
```sh
sudo apt purge cmake
```

Dal sito web scricare l'ultima versione (il tarball, .tar.gz): https://cmake.org/download/  
Io ho creato una cartella temp dove ho spostato il tar:
```sh
mkdir ~/temp
cd ~/temp
mv ../Scaricati/cmake-3.17.0-rc3.tar.gz ~/temp/ # Se hai mint in inglese sostituisci Scaricati con Download (credo)
tar -xzvf cmake-3.17.0-rc3.tar.gz
cd cmake-3.17.0-rc3/
./bootstrap 
make -j4
sudo make install
```

Controllare con:
```sh
cmake --version 
```

Output previsto:
```sh
cmake version 3.17.0-rc3

CMake suite maintained and supported by Kitware (kitware.com/cmake).
```

## ninja
Andare su :
https://github.com/ninja-build/ninja/releases/download/v1.10.0/ninja-linux.zip

Scaricare il file zip e poi:
```sh
cd Scaricati
sudo unzip ninja-linux.zip -d /usr/local/bin/
sudo update-alternatives --install /usr/bin/ninja ninja /usr/local/bin/ninja 1 --force
```

Per controllare se è stato installato correttamente:
```sh
ninja --version
```

Output previsto:
```sh
1.10.0
```

## OpenGl
Preso da: https://www.wikihow.com/Install-Mesa-(OpenGL)-on-Linux-Mint  
Open a terminal and enter the following commands to install the necessary libraries for OpenGL development:
```sh
sudo apt-get update
sudo apt-get install freeglut3
sudo apt-get install freeglut3-dev
sudo apt-get install binutils-gold
sudo apt-get install g++ cmake
sudo apt-get install libglew-dev
sudo apt-get install g++
sudo apt-get install mesa-common-dev
sudo apt-get install build-essential
sudo apt-get install libglew1.5-dev libglm-dev
```

## Embree
__ATTENZIONE!__  
Per disattivare embree:  
YOCTO_EMBREE and YOCTO_TESTING set to off on CMAKELIST.txt

Preso da: https://github.com/embree/embree  
Scaricare: https://github.com/embree/embree/releases/download/v3.8.0/embree-3.8.0.x86_64.rpm.tar.gz  

Dopo aver scaricato:
```sh
cd Scaricati
mv embree-3.8.0.x86_64.rpm.tar.gz ../Programmi ## Ho creato una cartella, di default non è presente
cd ../Programmi
tar xzf embree-3.8.0.x86_64.rpm.tar.gz
sudo apt-get install alien dpkg-dev debhelper build-essential

sudo alien embree3-lib-3.8.0-1.x86_64.rpm
sudo alien embree3-devel-3.8.0-1.noarch.rpm
sudo alien embree3-examples-3.8.0-1.x86_64.rpm

sudo dpkg -i embree3-lib_3.8.0-2_amd64.deb
sudo dpkg -i embree3-devel_3.8.0-2_all.deb
sudo dpkg -i embree3-examples_3.8.0-2_amd64.deb
sudo apt-get install libtbb-dev
```

## Modifiche da fare a yocto per funzoinare su Linux
Verdere le modifiche presenti su questo commit: https://github.com/xelatihy/yocto-gl/commit/cd6f2459e5bd7ff4ef45e846fc1fabc7857dae2f