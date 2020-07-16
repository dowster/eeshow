FROM ubuntu:20.04
RUN DEBIAN_FRONTEND=noninteractive apt-get update &&\
    apt-get install -y build-essential libgtk-3-dev libcairo2-dev libgit2-dev transfig \
  imagemagick git bsdmainutils -y -q

#COPY cairo-font.supp COPYING.GPLv2 db DEMO dl-init.supp eeshow-viewer file gfx gui help.txt icons kicad main main.h Makefile Makefile.c-common misc neo900-template.fig page_layout_default_description.cpp README sch2pdf test test.lib test.pro test.sch TODO version.c version.h web /eeshow/
COPY . /eeshow
WORKDIR eeshow

RUN make && PREFIX=/eeshow make install

FROM ubuntu:20.04
RUN apt-get update && \
    apt-get install -y transfig libgtk-3-0 libcairo2 libgit2-27 imagemagick git bsdmainutils
COPY --from=0 /eeshow/bin /usr/local/bin
CMD ["eeshow"]
