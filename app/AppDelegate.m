//
//  AppDelegate.m
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#include <resolv.h>
#include <arpa/inet.h>
#include <netdb.h>
#import "AppDelegate.h"
#import "TerminalViewController.h"
#include "kernel/init.h"
#include "kernel/calls.h"

@interface AppDelegate ()

@end

static void ios_handle_exit(int code) {
    [[NSNotificationCenter defaultCenter] postNotificationName:ISHExitedNotification object:nil];
}

@implementation AppDelegate

- (int)startThings {
    NSFileManager *manager = [NSFileManager defaultManager];
    NSURL *container = [manager containerURLForSecurityApplicationGroupIdentifier:@"group.com.worthdoingbadly.ish"];
    NSURL *alpineRoot = [container URLByAppendingPathComponent:@"roots/alpine"];
    [manager createDirectoryAtURL:[container URLByAppendingPathComponent:@"roots"]
      withIntermediateDirectories:YES
                       attributes:@{}
                            error:nil];
    if (![manager fileExistsAtPath:alpineRoot.path]) {
        NSURL *alpineMaster = [NSBundle.mainBundle URLForResource:@"alpine" withExtension:nil];
        NSError *error = nil;
        [manager copyItemAtURL:alpineMaster toURL:alpineRoot error:&error];
        if (error != nil) {
            NSLog(@"%@", error);
            exit(1);
        }
    }
    alpineRoot = [alpineRoot URLByAppendingPathComponent:@"data"];
    int err = mount_root(&fakefs, alpineRoot.fileSystemRepresentation);
    if (err < 0)
        return err;
    
    create_first_process();
    char *program = "/bin/bash";
    char *argv[] = {program, "-l", NULL};
    char *envp[] = {"TERM=xterm-256color", "PATH=/bin:/sbin:/usr/bin:/usr/sbin", NULL};
    err = sys_execve(program, argv, envp);
    if (err < 0)
        return err;
    err = create_stdio(ios_tty_driver);
    if (err < 0)
        return err;
    exit_hook = ios_handle_exit;
    
    // configure dns
    struct __res_state res;
    err = res_ninit(&res);
    NSMutableString *resolvConf = [NSMutableString new];
    for (int i = 0; res.dnsrch[i] != NULL; i++) {
        [resolvConf appendFormat:@"search %s\n", res.dnsrch[i]];
    }
    for (int i = 0; i < res.nscount; i++) {
        if (res.nsaddr_list[i].sin_len == 0)
            continue;
        char address[100];
        getnameinfo((struct sockaddr *) &res.nsaddr_list[i],
                    sizeof(res.nsaddr_list[i]), address,
                    sizeof(address), NULL, 0, NI_NUMERICHOST);
        [resolvConf appendFormat:@"nameserver %s\n", address];
    }
    struct fd *fd = generic_open("/etc/resolv.conf", O_WRONLY_ | O_CREAT_, 0600);
    if (!IS_ERR(fd)) {
        fd->ops->write(fd, resolvConf.UTF8String, [resolvConf lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
        fd_close(fd);
    }
    
    task_start(current);
    return 0;
}

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    int err = [self startThings];
    if (err < 0) {
        NSString *message = [NSString stringWithFormat:@"could not initialize"];
        NSString *subtitle = [NSString stringWithFormat:@"error code %d", err];
        if (err == _EINVAL)
            subtitle = [subtitle stringByAppendingString:@"\n(try reinstalling the app, see release notes for details)"];
        [self fatal:message subtitle:subtitle];
        NSLog(@"failed with code %d", err);
    }
    return YES;
}

- (void)fatal:(NSString *)message subtitle:(NSString *)subtitle {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:message message:subtitle preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"goodbye" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
            exit(0);
        }]];
        [self.window.rootViewController presentViewController:alert animated:YES completion:nil];
    });
}

@end

NSString *const ISHExitedNotification = @"ISHExitedNotification";
