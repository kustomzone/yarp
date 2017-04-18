/*
 * Copyright (C) 2006, 2008 RobotCub Consortium
 * Authors: Paul Fitzpatrick
 * CopyPolicy: Released under the terms of the LGPLv2.1 or later, see LGPL.TXT
 *
 */


#include <yarp/os/Network.h>
#include <yarp/serversql/yarpserversql.h>

#include <stdio.h>

using namespace yarp::os;


int main(int argc, char *argv[]) 
{
    yarp::os::Network::forceSystemClock();     // Yarp server must always run using system clock
    // intercept "yarp server" if needed
    if (argc>=2) {
        if (ConstString(argv[1])=="server") {
            return yarpserver_main(argc,argv);
        }
    }

    std::cout << "hello!!";
    // call the yarp standard companion
    Network yarp;
    return Network::main(argc,argv);
}

